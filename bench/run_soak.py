#!/usr/bin/env python3
"""Holds a fleet against the cluster for a long run and reports drift.

The sweep answers what the cluster does for forty five seconds. This answers
whether it still does it twenty minutes later: tick time, resident players,
entity count, receive drops and container memory are sampled throughout, and
the report is the trend of each rather than its average, because a leak and a
slow degradation both look fine in a single scrape taken at the end.
"""

import argparse
import json
import subprocess
import sys
import threading
import time
import urllib.request

SHARDS = ["world-a", "world-b", "world-c", "world-d"]
PORTS = {name: 40081 + index for index, name in enumerate(SHARDS)}


def fetch(url, timeout=10):
    with urllib.request.urlopen(url, timeout=timeout) as response:
        return response.read().decode()


def scalar(text, name):
    for line in text.splitlines():
        if line.startswith(name + "{") or line.startswith(name + " "):
            return float(line.split()[-1])
    return 0.0


def interval_means(points, shard):
    """Tick means for each gap between samples.

    The exported histogram is cumulative, so its mean is an average over the
    whole run so far and a shard that doubled its tick time in the last minute
    would barely move it. Differencing the sum and the count recovers what each
    interval actually cost.
    """
    out = []
    for before, after in zip(points, points[1:]):
        span = after["shards"][shard]["tick_count"] - before["shards"][shard]["tick_count"]
        if span <= 0:
            continue
        total = after["shards"][shard]["tick_sum"] - before["shards"][shard]["tick_sum"]
        out.append(total / span * 1000.0)
    return out


def container_memory():
    text = subprocess.run(
        ["docker", "stats", "--no-stream", "--format", "{{.Name}} {{.MemUsage}}"],
        capture_output=True, text=True).stdout
    out = {}
    for line in text.splitlines():
        parts = line.split()
        if not parts or not parts[0].startswith("morton-world-"):
            continue
        value = parts[1]
        scale = 1.0 if value.endswith("MiB") else (1024.0 if value.endswith("GiB") else 1 / 1024.0)
        out[parts[0].replace("morton-", "").rsplit("-", 1)[0]] = round(
            float(value.rstrip("KMGiB")) * scale, 1)
    return out


def sample():
    """One observation of the whole fleet, taken from the shards themselves."""
    point = {"wall": round(time.time(), 1), "shards": {}}
    memory = container_memory()
    for name, port in PORTS.items():
        text = fetch(f"http://127.0.0.1:{port}/metrics")
        point["shards"][name] = {
            "tick_sum": scalar(text, "morton_tick_seconds_sum"),
            "tick_count": scalar(text, "morton_tick_seconds_count"),
            "players": scalar(text, "morton_players"),
            "redirecting": scalar(text, "morton_players_redirecting"),
            "entities": scalar(text, "morton_entities"),
            "receive_drops": scalar(text, "morton_receive_drops_total"),
            "snapshots": scalar(text, "morton_snapshots_total"),
            "memory_mib": memory.get(name, 0.0),
        }
    return point


def collect(stop, into, interval):
    while not stop.is_set():
        try:
            into.append(sample())
        except Exception as error:
            print(f"  sample failed: {error}", file=sys.stderr, flush=True)
        stop.wait(interval)


def series(points, shard, field):
    return [point["shards"][shard][field] for point in points]


def report(points, window):
    """Compares the first and last window of samples, per shard."""
    if len(points) < 2 * window:
        window = max(1, len(points) // 3)

    print(f"\n{len(points)} samples over "
          f"{points[-1]['wall'] - points[0]['wall']:.0f}s\n")
    print(f"{'shard':9} {'tick early':>11} {'tick late':>10} {'players':>8} "
          f"{'entities':>9} {'mem early':>10} {'mem late':>9} {'drops':>9}")

    worst_drift = 0.0
    for shard in SHARDS:
        ticks = interval_means(points, shard)
        if len(ticks) < 2:
            continue
        span = min(window, len(ticks) // 2)
        early = sum(ticks[:span]) / span
        late = sum(ticks[-span:]) / span
        memory = series(points, shard, "memory_mib")
        players = series(points, shard, "players")
        entities = series(points, shard, "entities")
        drops = series(points, shard, "receive_drops")
        drift = (late - early) / early * 100.0 if early else 0.0
        worst_drift = max(worst_drift, abs(drift))
        print(f"{shard:9} {early:10.2f}m {late:9.2f}m {max(players):8.0f} "
              f"{max(entities):9.0f} {memory[0]:9.1f}M {memory[-1]:8.1f}M "
              f"{drops[-1] - drops[0]:9.0f}")

    print(f"\nworst tick drift across shards: {worst_drift:.1f}%")
    return worst_drift


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--clients", type=int, default=2000)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--duration", type=int, default=600)
    parser.add_argument("--ramp", type=int, default=700)
    parser.add_argument("--interval", type=int, default=15)
    parser.add_argument("--window", type=int, default=4)
    parser.add_argument("--out", default="bench/soak.json")
    args = parser.parse_args()

    points = []
    stop = threading.Event()
    sampler = threading.Thread(target=collect, args=(stop, points, args.interval), daemon=True)

    print(f"soaking {args.clients} clients for {args.duration}s", flush=True)
    sampler.start()
    client = subprocess.run(
        ["docker", "compose", "run", "--rm", "--no-deps", "-T",
         "-e", f"MORTON_CLIENTS={args.clients}",
         "-e", f"MORTON_THREADS={args.threads}",
         "-e", f"MORTON_DURATION={args.duration}",
         "-e", f"MORTON_RAMP={args.ramp}",
         "loadtest", "--json"],
        capture_output=True, text=True)
    stop.set()
    sampler.join(timeout=args.interval + 5)

    fleet = {}
    for line in reversed(client.stdout.splitlines()):
        if line.strip().startswith("{"):
            fleet = json.loads(line)
            break

    if not points:
        raise SystemExit("no samples were taken:\n" + client.stdout + client.stderr)

    drift = report(points, args.window)
    if fleet:
        print(f"fleet peak {fleet['clients_peak_connected']}  "
              f"loss {fleet['loss_mean_percent']:.2f}%  "
              f"rtt p99 {fleet['rtt_p99_ms']:.1f} ms  "
              f"rejoins {fleet.get('rejoins', 0)}")

    with open(args.out, "w") as handle:
        json.dump({"samples": points, "fleet": fleet, "tick_drift_percent": drift}, handle, indent=2)
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
