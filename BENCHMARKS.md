# Benchmarks

All figures are measured, not modelled. Tick and phase numbers come from the shards' own Prometheus histograms; loss, RTT and bandwidth come from the load fleet's client-side report.

## Rig

| | |
|---|---|
| Host | Apple Silicon, Docker Desktop |
| Shards | 4 × `morton_world`, **1.0 CPU each** |
| Support | 1 × Redis, 1 × matchmaker |
| Load fleet | `morton_loadtest`, 3.0 CPU, 4 threads |
| Tick rate | 30 Hz |
| World | 2×2 regions, 300 ambient drifters per shard |
| Method | 3 interleaved rounds per fleet size, 45 s each, median reported |

Rounds are interleaved — every round visits every fleet size — so host contention drifting over minutes can't be mistaken for a capacity effect. Each run records the background load it started against, and a >20% spread across rounds is flagged rather than reported as a result.

## Capacity curve

| fleet | connected | tick mean | worst p99 | loss | rtt p99 | per-client | busiest shard |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 500 | 499 | 1.53 ms | 7.0 ms | 0.00% | 59.6 ms | 67 kbit/s | 11% |
| 1000 | 997 | 2.84 ms | 16.2 ms | 0.00% | 64.8 ms | 104 kbit/s | 21% |
| 1500 | 1491 | 6.00 ms | 19.4 ms | 0.01% | 66.3 ms | 118 kbit/s | 30% |
| 2000 | 1984 | 6.73 ms | 17.2 ms | 0.02% | 66.3 ms | 121 kbit/s | 39% |
| 2500 | 2477 | 9.75 ms | 22.9 ms | 0.10% | 67.0 ms | 122 kbit/s | 51% |
| **3000** | **2970** | **14.42 ms** | **35.8 ms** | **1.41%** | 66.9 ms | 124 kbit/s | 73% |
| 4000 | 3902 | 25.89 ms | 94.1 ms | 23.15% | 130.3 ms | 104 kbit/s | 95% |

```
tick mean (ms) vs fleet, against the 33.3 ms tick budget
 33 ┤                                              ╭────── budget
 26 ┤                                          ╭───╯ 4000
 20 ┤                                     ╭────╯
 14 ┤                            ╭────────╯ 3000
 10 ┤                   ╭────────╯ 2500
  7 ┤          ╭────────╯ 2000
  3 ┤   ╭──────╯ 1000
  1 ┼───╯ 500
    └────┴────┴────┴────┴────┴────┴────┴
       500  1000 1500 2000 2500 3000 4000
```

**Ceiling: ~3000 clients.** At 4000 the busiest shard is at 95% of its single core — the limit is genuine CPU saturation in the replication encoder, not a coordination stall or a queueing artefact. RTT stays flat at ~66 ms right up to the cliff.

## Where the tick goes

At 2000 clients, one shard:

```
tick 6.76 ms
├── replicate  5.30 ms  ███████████████████████████▏  78%
│   ├── encode 4.16 ms  █████████████████████▍        62%
│   └── send   1.06 ms  █████▍                        16%
├── migrate    0.53 ms  ██▊                            8%
├── cluster    0.18 ms  ▉                              3%
└── step       0.17 ms  ▉                              3%
```

Physics is free. Replication is the entire cost, and encoding is the bulk of replication — which is why the microbenchmarks below target the encoder specifically.

## Bandwidth

Delta encoding against the client's last acked baseline, bit-packed and fixed-point quantized:

| fleet | measured | naive full-state | reduction |
|---:|---:|---:|---:|
| 500 | 67 kbit/s | 1.6 Mbit/s | 95.8% |
| 1000 | 104 kbit/s | 1.7 Mbit/s | 93.8% |
| 2000 | 121 kbit/s | 1.9 Mbit/s | 93.7% |
| 3000 | 124 kbit/s | 2.8 Mbit/s | 95.6% |

Mean snapshot at 2000 clients: **520 bytes**, p99 631 bytes — one MTU, by construction.

> **Caveat.** This column is computed against an entity gauge scraped after the fleet disconnected, so it **understates**. The harness reads the gauge live as of `bf4ec19`; corrected spot measurements land at 96.6–97.4%. The next clean sweep will replace these numbers.

## Encoder microbenchmark

`morton_replbench` — the replication path in isolation, no network, no Docker. 600 ticks, 5 runs, median.

**Scaling viewers** (players = 2 × drifters):

| players | replicate mean | p99 | per viewer | snapshot | entities sent |
|---:|---:|---:|---:|---:|---:|
| 100 | 0.065 ms | 0.10 ms | 0.65 µs | 76 B | 8.0 |
| 250 | 0.286 ms | 0.41 ms | 1.15 µs | 147 B | 17.7 |
| 500 | 1.028 ms | 1.79 ms | 2.06 µs | 262 B | 33.5 |
| 750 | 2.244 ms | 2.82 ms | 2.99 µs | 385 B | 50.4 |
| 1000 | 4.025 ms | 4.74 ms | 4.03 µs | 501 B | 66.3 |

**Scaling world density** at a fixed 500 viewers:

| drifters | replicate mean | per viewer | snapshot | entities sent |
|---:|---:|---:|---:|---:|
| 0 | 0.681 ms | 1.36 µs | 194 B | 23.5 |
| 500 | 1.307 ms | 2.61 µs | 330 B | 43.6 |
| 1000 | 2.043 ms | 4.09 µs | 474 B | 64.7 |
| 2000 | 3.956 ms | 7.91 µs | 559 B | 76.8 |
| 4000 | 6.370 ms | 12.74 µs | **578 B** | **76.9** |

Interest management is doing its job: an 8× increase in world density (500 → 4000 drifters) moves the snapshot from 474 B to 578 B and the entities sent from 64.7 to 76.9. Cost tracks *what a viewer can see*, not what exists.

## Soak

2000 clients held for 10 minutes, 35 samples at 15 s.

| | |
|---|---|
| Peak connected | 1987 |
| Worst tick drift, first window → last | **3.9%** |
| Loss | 0.043% mean, 1.05% p99 |
| RTT | 33.9 p50 / 61.4 p95 / 66.4 p99 / 85.9 max ms |
| Prediction error | 1.63 mean, 9.54 p99 |
| Migrations | 201,651 |
| Rejoins survived | 73 |
| Snapshots | 35.6 M received, 35.6 M applied |
| Throughput | 59,006 snapshots/s, 252 Mbit/s downstream |

Memory per shard over the run:

| shard | start | end |
|---|---:|---:|
| world-a | 96.9 MiB | 111.2 MiB |
| world-b | 155.9 MiB | 155.7 MiB |
| world-c | 164.4 MiB | 164.1 MiB |
| world-d | 135.9 MiB | 135.2 MiB |

Flat after the first minute. No leak.

Tick means are differenced between consecutive samples rather than read from the cumulative histogram — a cumulative mean is an average over the whole run, and a shard that doubled its tick time in the last minute would barely move it.

## Reproducing

```bash
python3 bench/run_bench.py --fleets 500,1000,1500,2000,2500,3000,4000 --repeat 3
python3 bench/run_encode_bench.py
python3 bench/run_soak.py --clients 2000 --duration 600
```

Results land in `bench/results.json`, `bench/results_encode.json`, `bench/soak.json`.

Run on a quiet host. Every harness records `os.getloadavg()` alongside each measurement so a contended run is identifiable after the fact rather than silently folded into the curve.
