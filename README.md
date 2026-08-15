# Morton

A distributed, authoritative real-time simulation backend in C++20 — the server infrastructure a multiplayer game runs on. Fixed-tick authoritative simulation over UDP, client prediction with server reconciliation, interest-managed delta replication, and live player migration across a sharded world.

**~3000 concurrent clients at 30 Hz across four single-core shards**, at 0.1% packet loss and a 22.9 ms worst-shard tick p99.

| | |
|---|---|
| [ARCHITECTURE.md](ARCHITECTURE.md) | processes, tick loop, cluster coordination, failure handling |
| [PROTOCOL.md](PROTOCOL.md) | packet frames, handshake, reliability, snapshot encoding |
| [BENCHMARKS.md](BENCHMARKS.md) | capacity curve, phase breakdown, encoder microbenchmarks, soak |

---

## Topology

```
                        ┌──────────────┐
        client ────────▶│  matchmaker  │  picks a shard, issues a signed token
                        └───────┬──────┘
                                │
              ┌─────────────────┼─────────────────┐
              ▼                 ▼                 ▼
        ┌──────────┐      ┌──────────┐      ┌──────────┐
        │ world-a  │◀────▶│ world-b  │◀────▶│  world-c │   ... world-d
        │ region 0 │      │ region 1 │      │ region 2 │
        └────┬─────┘      └────┬─────┘      └────┬─────┘
             │                 │                 │
             └─────────────────┼─────────────────┘
                               ▼
                        ┌──────────────┐
                        │    Redis     │  presence, shard liveness,
                        └──────────────┘  migration tickets
```

Each shard owns one region of a shared world and simulates it authoritatively. A player who walks across a border is handed to the neighbouring shard mid-session, carrying position, velocity and input sequence with them — no reconnect, no respawn.

## Tick

One shard tick, 30 Hz, single-threaded:

```
receive ──▶ step ──▶ replicate ──▶ encode ──▶ send ──▶ migrate ──▶ cluster
 inputs    physics    AOI query    delta+bits   UDP    handoff    presence
                                                                     │
                        ◀──────── drain socket until next tick ──────┘
```

The idle window between ticks is spent reading the socket rather than sleeping, which keeps the kernel receive queue shallow at fleet scale.

| Phase | mean @ 2000 clients | p99 |
|---|---|---|
| step | 0.17 ms | 0.30 ms |
| replicate | 5.30 ms | 10.56 ms |
| ├ encode | 4.16 ms | 8.07 ms |
| └ send | 1.06 ms | 2.24 ms |
| migrate | 0.53 ms | 2.82 ms |
| cluster | 0.18 ms | 2.48 ms |
| **tick** | **6.76 ms** | **14.70 ms** |

## What's in the box

| Layer | Implementation |
|---|---|
| **Transport** | UDP with salted 3-way handshake, sequence numbers, 32-bit piggybacked ack bitfield, reliable-ordered channel for events, `sendmmsg`/`recvmmsg` batching with portable fallback |
| **Simulation** | Fixed 30 Hz authoritative step, deterministic movement, per-player input buffers |
| **Prediction** | Client predicts locally, replays unacked inputs against each authoritative snapshot; measured mean error 1.6 units |
| **Interest** | Morton/Z-order spatial hash, per-viewer AOI query, priority accumulator so distant entities can't be starved indefinitely |
| **Replication** | Delta snapshots against the client's last acked baseline, bit-packed, fixed-point quantized — **93–96% smaller** than naive full-state |
| **Cluster** | Redis-backed: TTL shard liveness, Lua compare-and-set presence claims, atomic `GETDEL` migration tickets, pipelined roster heartbeat |
| **Migration** | Hysteresis dead band on region borders, redirect grace window, ticket redemption on the destination shard |
| **Resilience** | Reconnect grace resumes the same avatar in place; shard loss is detected by TTL expiry and rerouted |
| **Observability** | Prometheus exposition, lock-free geometric-bucket histograms on every tick phase |
| **Viewer** | Browser client served from the binary, live WebSocket world state |

## Run it

```bash
docker compose build world-a
docker compose up -d redis matchmaker world-a world-b world-c world-d
```

Then open the viewer:

```
http://127.0.0.1:40081/?ws=127.0.0.1:40091,127.0.0.1:40092,127.0.0.1:40093,127.0.0.1:40094
```

Four coloured regions, live avatars, per-shard tick stats in the corner.

**Load test** — thousands of real clients, each running the full handshake, prediction and reconciliation path:

```bash
docker compose run --rm loadtest \
  -e MORTON_CLIENTS=2000 -e MORTON_THREADS=4 -e MORTON_DURATION=60
```

**Build and test locally:**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build -j
cd build && ctest --output-on-failure    # 16 suites
```

Requires CMake 3.16+ and a C++20 compiler. No third-party dependencies — the Redis client, HTTP server, WebSocket server and test harness are all in-tree. `-DMORTON_ASAN=ON` builds under ASan/UBSan.

## Binaries

| Binary | Role |
|---|---|
| `morton_world` | One authoritative shard |
| `morton_matchmaker` | Shard selection and token issuance |
| `morton_loadtest` | Threaded fake-player fleet |
| `morton_replbench` | Replication encoder microbenchmark |
| `morton_sockbench` | Socket throughput microbenchmark |

Configured entirely by environment: `MORTON_SHARD_ID`, `MORTON_UDP`, `MORTON_HTTP`, `MORTON_WS`, `MORTON_REDIS`, `MORTON_TICK_RATE`, `MORTON_CAPACITY`, `MORTON_DRIFTERS`, `MORTON_REGIONS_PER_AXIS`, `MORTON_VIEWER_HZ`.

## Endpoints

| Path | Serves |
|---|---|
| `/health` | shard liveness |
| `/metrics` | Prometheus exposition |
| `/state` | current world state as JSON |
| `/shards` | (matchmaker) live shard listing |
| `/` | browser viewer |

## Layout

```
src/net/       UDP, reliability, HTTP, WebSocket
src/proto/     bitstream, snapshot encoding, wire messages
src/sim/       world, movement, regions, per-client view
src/spatial/   Morton codes, spatial hash grid
src/cluster/   Redis client, presence, migration, matchmaker
src/metrics/   histograms, Prometheus registry
src/app/       world server, game client, load test
apps/          binary entry points
bench/         benchmark harnesses and recorded results
tests/         16 suites
```
