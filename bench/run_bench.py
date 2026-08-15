#!/usr/bin/env python3
"""Runs the load fleet against the compose cluster and reports measured results.

Sweeps a list of fleet sizes, and for each one records the load generator's
client-side report alongside every shard's tick, replication and bandwidth
histograms scraped from Prometheus, so the tick numbers come from the shards
themselves rather than from the client's view of them.
"""

import argparse
import json
import math
import os
import subprocess
import sys
import time
import urllib.request

SHARDS = ["world-a", "world-b", "world-c", "world-d"]
HISTOGRAMS = [
    "morton_tick_seconds",
    "morton_step_seconds",
    "morton_replicate_seconds",
    "morton_encode_seconds",
    "morton_send_seconds",
    "morton_net_seconds",
    "morton_migrate_seconds",
    "morton_snapshot_bytes",
]


def compose(*args, capture=False, check=True):
    command = ["docker", "compose", *args]
    if capture:
        return subprocess.run(command, capture_output=True, text=True, check=check).stdout
    subprocess.run(command, check=check)
    return ""


def fetch(url, timeout=10):
    with urllib.request.urlopen(url, timeout=timeout) as response:
        return response.read().decode()


def parse_metrics(text):
    buckets, sums, counts = {}, {}, {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        name = line.split("{")[0].split(" ")[0]
        value = float(line.split()[-1])
        if name.endswith("_bucket"):
            bound = float(line.split('le="')[1].split('"')[0])
            buckets.setdefault(name[:-7], []).append((bound, value))
        elif name.endswith("_sum"):
            sums[name[:-4]] = value
        elif name.endswith("_count"):
            counts[name[:-6]] = value
        else:
            sums.setdefault(name, value)
    for series in buckets.values():
        series.sort()
    return buckets, sums, counts


def quantile(series, count, q):
    if not series or count == 0:
        return 0.0
    target = q * count
    previous_bound, previous_count = 0.0, 0.0
    for bound, cumulative in series:
        if cumulative >= target:
            if math.isinf(bound):
                return previous_bound
            span = cumulative - previous_count
            if span <= 0:
                return bound
            fraction = (target - previous_count) / span
            return previous_bound + (bound - previous_bound) * fraction
        previous_bound, previous_count = bound, cumulative
    return series[-1][0]


def shard_report(port):
    buckets, sums, counts = parse_metrics(fetch(f"http://127.0.0.1:{port}/metrics"))
    report = {}
    for name in HISTOGRAMS:
        count = counts.get(name, 0.0)
        if count == 0:
            continue
        scale = 1000.0 if name.endswith("_seconds") else 1.0
        report[name] = {
            "count": count,
            "mean": sums.get(name, 0.0) / count * scale,
            "p50": quantile(buckets.get(name, []), count, 0.50) * scale,
            "p95": quantile(buckets.get(name, []), count, 0.95) * scale,
            "p99": quantile(buckets.get(name, []), count, 0.99) * scale,
        }
    report["players"] = sums.get("morton_players", 0.0)
    report["entities"] = sums.get("morton_entities", 0.0)
    report["snapshots_total"] = sums.get("morton_snapshots_total", 0.0)
    report["snapshot_bytes_total"] = sums.get("morton_snapshot_bytes_total", 0.0)
    report["migrations_out"] = sums.get("morton_migrations_out_total", 0.0)
    report["migrations_in"] = sums.get("morton_migrations_in_total", 0.0)
    return report


def wait_for_cluster(timeout_s=120):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            listing = json.loads(fetch("http://127.0.0.1:8080/shards"))
            if listing.get("count", 0) == len(SHARDS):
                return listing
        except Exception:
            pass
        time.sleep(1)
    raise SystemExit("cluster did not become ready")


def run_fleet(clients, threads, duration, ramp):
    output = compose(
        "run", "--rm", "--no-deps", "-T",
        "-e", f"MORTON_CLIENTS={clients}",
        "-e", f"MORTON_THREADS={threads}",
        "-e", f"MORTON_DURATION={duration}",
        "-e", f"MORTON_RAMP={ramp}",
        "loadtest", "--json",
        capture=True,
    )
    for line in reversed(output.splitlines()):
        line = line.strip()
        if line.startswith("{"):
            return json.loads(line)
    raise SystemExit("load test produced no report:\n" + output)


def summarize(fleet, client, shards):
    """Folds one run's client report and shard scrapes into a result record."""
    ticks = [s["morton_tick_seconds"] for s in shards.values() if "morton_tick_seconds" in s]
    entities = max(s["entities"] for s in shards.values())
    uncompressed_kbps = entities * 20.0 * 8.0 * 30.0 / 1000.0
    measured_kbps = client["client_recv_kbps_mean"]
    return {
        "fleet": fleet,
        "client": client,
        "shards": shards,
        "tick_mean_ms": sum(t["mean"] for t in ticks) / len(ticks) if ticks else 0.0,
        "worst_tick_p99_ms": max(t["p99"] for t in ticks) if ticks else 0.0,
        "entities_per_shard": entities,
        "uncompressed_kbps": uncompressed_kbps,
        "bandwidth_reduction_percent":
            100.0 * (1.0 - measured_kbps / uncompressed_kbps) if uncompressed_kbps else 0.0,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--fleets", default="250,500,1000,2000")
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--duration", type=int, default=45)
    parser.add_argument("--ramp", type=int, default=500)
    parser.add_argument("--out", default="bench/results.json")
    parser.add_argument("--keep-up", action="store_true")
    parser.add_argument("--repeat", type=int, default=3)
    args = parser.parse_args()

    compose("build", "world-a")
    compose("up", "-d", "redis", "matchmaker", *SHARDS)
    wait_for_cluster()

    fleets = [int(value) for value in args.fleets.split(",") if value]
    attempts = {fleet: [] for fleet in fleets}

    # Host contention drifts over minutes, so measuring one fleet three times
    # before starting the next confounds fleet size with whatever else the
    # machine happened to be doing. Every round visits every fleet instead, and
    # each run records the background load it started against.
    for attempt in range(args.repeat):
        for fleet in fleets:
            compose("restart", *SHARDS)
            wait_for_cluster()
            time.sleep(3)
            load = round(os.getloadavg()[0], 2)

            client = run_fleet(fleet, args.threads, args.duration, args.ramp)
            shards = {name: shard_report(40081 + index) for index, name in enumerate(SHARDS)}
            record = summarize(fleet, client, shards)
            record["host_load"] = load
            attempts[fleet].append(record)
            print(f"  round {attempt + 1}/{args.repeat}  fleet {fleet}  "
                  f"peak {record['client']['clients_peak_connected']}  "
                  f"tick mean {record['tick_mean_ms']:.2f} ms  "
                  f"p99 {record['worst_tick_p99_ms']:.2f} ms  "
                  f"loss {record['client']['loss_mean_percent']:.2f}%  "
                  f"host load {load}", flush=True)

    results = []
    for fleet in fleets:
        print(f"\n=== fleet of {fleet} clients ===", flush=True)
        runs = sorted(attempts[fleet], key=lambda run: run["tick_mean_ms"])
        chosen = runs[len(runs) // 2]
        chosen["tick_mean_ms_runs"] = [round(run["tick_mean_ms"], 3) for run in runs]
        chosen["host_load_runs"] = [run["host_load"] for run in runs]
        spread = runs[-1]["tick_mean_ms"] - runs[0]["tick_mean_ms"]
        chosen["tick_mean_spread_percent"] = round(
            100.0 * spread / chosen["tick_mean_ms"], 1) if chosen["tick_mean_ms"] else 0.0
        results.append(chosen)

        # Runs of the same build on an uncontended host land within a few percent
        # of each other, so a wide spread means the number describes the machine
        # rather than the server and should not be read as a capacity result.
        if chosen["tick_mean_spread_percent"] > 20.0:
            print(f"  WARNING: tick mean varied {chosen['tick_mean_spread_percent']:.0f}% "
                  f"across runs; the host was contended", flush=True)

        client = chosen["client"]
        print(f"connected peak {client['clients_peak_connected']}/{fleet}  "
              f"rtt p99 {client['rtt_p99_ms']:.1f} ms  "
              f"loss {client['loss_mean_percent']:.2f}%  "
              f"per-client {client['client_recv_kbps_mean']:.1f} kbit/s  "
              f"reduction {chosen['bandwidth_reduction_percent']:.2f}%  "
              f"worst shard tick p99 {chosen['worst_tick_p99_ms']:.2f} ms  "
              f"migrations {client['migrations']}", flush=True)

    with open(args.out, "w") as handle:
        json.dump(results, handle, indent=2)
    print(f"\nwrote {args.out}")

    if not args.keep_up:
        compose("down", "-v")
    return 0


if __name__ == "__main__":
    sys.exit(main())
