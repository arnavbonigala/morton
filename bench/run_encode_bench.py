#!/usr/bin/env python3
"""Characterises the snapshot encoder on one core, away from the cluster.

The compose sweep measures a whole fleet and so inherits whatever else the host
is doing. This drives morton_replbench directly instead, which makes the encode
cost per viewer reproducible enough to compare builds against.
"""

import argparse
import json
import subprocess
import sys

BINARY = "build-arm64/apps/morton_replbench"


def measure(binary, players, drifters, ticks, repeat):
    runs = []
    for attempt in range(repeat):
        output = subprocess.run(
            [binary,
             "--MORTON_PLAYERS", str(players),
             "--MORTON_DRIFTERS", str(drifters),
             "--MORTON_TICKS", str(ticks),
             "--MORTON_SEED", str(1234 + attempt),
             "--json"],
            capture_output=True, text=True, check=True).stdout
        runs.append(json.loads(output.strip().splitlines()[-1]))

    runs.sort(key=lambda run: run["per_viewer_us"])
    chosen = runs[len(runs) // 2]
    chosen["per_viewer_us_runs"] = [round(run["per_viewer_us"], 3) for run in runs]
    return chosen


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default=BINARY)
    parser.add_argument("--ticks", type=int, default=600)
    parser.add_argument("--repeat", type=int, default=5)
    parser.add_argument("--out", default="bench/results_encode.json")
    args = parser.parse_args()

    # Two axes: how the cost grows with the number of viewers at a fixed world
    # density, and how it grows with the number of entities each viewer sees.
    grid = ([(players, players // 2) for players in (100, 250, 500, 750, 1000)] +
            [(500, drifters) for drifters in (0, 500, 1000, 2000, 4000)])

    results = []
    for players, drifters in grid:
        run = measure(args.binary, players, drifters, args.ticks, args.repeat)
        results.append(run)
        print(f"players {players:5d}  drifters {drifters:5d}  "
              f"per viewer {run['per_viewer_us']:6.3f} us  "
              f"replicate mean {run['replicate_mean_ms']:6.3f} ms  "
              f"sent {run['entities_sent_mean']:6.1f}  "
              f"{run['snapshot_mean_bytes']:6.1f} B  "
              f"{run['per_client_kbps']:6.1f} kbit/s  "
              f"runs {run['per_viewer_us_runs']}", flush=True)

    with open(args.out, "w") as handle:
        json.dump(results, handle, indent=2)
    print(f"\nwrote {args.out}")


if __name__ == "__main__":
    sys.exit(main())
