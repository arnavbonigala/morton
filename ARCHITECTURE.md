# Architecture

How Morton is put together, and why each piece is shaped the way it is. Wire formats live in [PROTOCOL.md](PROTOCOL.md); measurements live in [BENCHMARKS.md](BENCHMARKS.md).

---

## Processes

| Process | Owns | State |
|---|---|---|
| `morton_world` | one region of the world, authoritatively | in-memory simulation |
| `morton_matchmaker` | shard selection, token issuance | stateless |
| `redis` | presence, liveness, migration tickets | TTL-scoped only |

Nothing is persisted. Every piece of shared state carries a TTL, so a killed process and a cleanly stopped one are indistinguishable and no reaper has to run.

## Threading

Each shard is **single-threaded for simulation**. One thread runs the tick loop end to end: receive, step, replicate, encode, send, migrate, cluster.

| Concern | Handling |
|---|---|
| Simulation | tick thread, exclusively |
| HTTP `/metrics`, `/health`, `/state` | separate thread, lock-free histogram reads |
| WebSocket viewer | separate thread, snapshot published by the tick thread |
| Redis | tick thread, pipelined so a round trip is paid once, not per player |

No locks on the hot path. The metrics histograms are lock-free with geometric buckets precisely so the scrape thread never contends with the tick.

## The tick

```
                    ┌─────────────────────────────────────────┐
                    │              33.3 ms budget             │
                    └─────────────────────────────────────────┘

  receive ──▶ step ──▶ replicate ──▶ migrate ──▶ cluster ──▶ drain until next
     │         │           │            │           │              │
  decode    integrate   per-viewer   border      presence     read socket
  inputs    physics     AOI+encode   handoff     heartbeat    while idle
                                                                   │
     ◀─────────────────────────────────────────────────────────────┘
```

### Why the idle window is spent reading

A shard's kernel receive queue holds only a few hundred datagrams once per-datagram overhead is charged, and Linux silently clamps `SO_RCVBUF` against `net.core.rmem_max`. A fleet of 625 clients at 30 Hz delivers ~625 datagrams *between two ticks*. Draining only at the top of the tick therefore overflows the queue in the **steady state**, not just during a spike — and the inputs the kernel discards look to the client exactly like packet loss.

So the shard reads through the idle window in ≤2 ms slices instead of sleeping. Inputs land in the same per-player buffers either way and are applied by the next step; the queue just never gets deep.

This is instrumented, not assumed: `SO_RXQ_OVFL` makes the kernel attach a cumulative drop counter to each delivered datagram, exported as `morton_receive_drops_total`. A silent drop is a diagnosable one.

### Where the time goes

At 2000 clients, replication is 78% of the tick and physics is 3%. The encoder is the thing worth optimising, and the only thing. Full breakdown in [BENCHMARKS.md](BENCHMARKS.md).

## World and space

```
World (2048 units, 2×2 regions)
│
├── Entity storage — structure-of-arrays: position[], velocity[], kind[]
│
└── Spatial hash — Morton/Z-order coded cells
        │
        └── AOI query: radius 260 → candidate entities
```

Z-order coding means a 2D neighbourhood query walks a **contiguous-ish** range of cell codes rather than scattering across a hash table, so the AOI query for adjacent viewers touches overlapping cache lines.

Entities are stored as parallel arrays, and the replication encoder reads `position[i]` / `velocity[i]` / `kind[i]` in id order — a linear scan, not a pointer chase.

## Replication

Per viewer, per tick:

```
 AOI query ──▶ quantize ──▶ diff vs acked baseline ──▶ prioritize ──▶ fill budget
                  │                    │                    │             │
            wire resolution      linear merge          carried        1000 B,
            (16-bit position)    over sorted ids       priority       one MTU
                  │                    │                    │
                  └── unchanged after quantization = costs nothing ──┘
```

The design decision that matters: **diff on the quantized state, not the float state.** An entity whose float position wobbled below the wire resolution is genuinely unchanged as far as the wire is concerned, and costs zero bits. Diffing floats would send it every tick.

Deferred entities keep accruing priority, so a distant entity that loses the race for a full packet several ticks running eventually outranks closer ones. Interest management without that credit is a starvation bug waiting for a dense enough world.

## Cluster coordination

### Ownership is derived, not elected

```
        Redis: live shard set (TTL-scoped)
                     │
        ┌────────────┼────────────┐
        ▼            ▼            ▼
    world-a      world-b      world-c        each sorts the same list,
        │            │            │          maps regions onto it,
        └────────────┴────────────┘          and reaches the same answer
                     │
              no consensus round
```

Every shard maps regions onto the sorted live-shard list from the same registry snapshot. They agree without a consensus round, without an election, and without a leader to lose. A dead shard's regions are simply reassigned by the next refresh.

### Liveness is TTL-driven

| | |
|---|---|
| Shard heartbeat TTL | 4 s |
| Refresh interval | 1 s |
| Presence TTL | 15 s |
| Migration ticket TTL | 10 s |

A shard that stops heartbeating expires. There is no health-check sweep, no failure detector, and no distinction between crashed and stopped. Membership entries left by expired shards are pruned as a side effect of discovery, so nothing accumulates.

### The heartbeat is pipelined

Refreshing presence one player at a time is four Redis commands per player, each a round trip, all inside the tick. At 620 residents that is the longest thing the tick does — by a wide margin, and it scales with fleet size exactly the way a capacity ceiling does.

Instead the whole roster is queued and flushed once:

```
for each resident:  queue HSET, PEXPIRE, SADD, PEXPIRE
                                 │
                                 ▼
                         one flush, one round trip
```

Cost at 2000 clients: **0.18 ms mean, 2.48 ms p99**, tracked in its own `morton_cluster_seconds` histogram so it can never again hide between the measured phases.

## Migration

A player crossing a region border, end to end:

```
 world-a                        Redis                        world-b
    │                             │                             │
    │ position past border        │                             │
    │ + 48-unit hysteresis        │                             │
    │                             │                             │
    │── SET ticket + transfer ───▶│  one round trip, CAS on     │
    │   presence (atomic)         │  from_shard                 │
    │                             │                             │
    │── redirect event ──────────▶ client                       │
    │   (reliable channel)          │                           │
    │                               │── connect + ticket ──────▶│
    │  keeps simulating for         │                           │
    │  45 ticks (redirect grace)    │◀── GETDEL redeem ─────────│
    │                               │                           │
    │                               │◀── accepted, resumed ─────│
```

| Guard | Against |
|---|---|
| 48-unit hysteresis dead band | ping-ponging across the border |
| 45-tick redirect grace | the reliable redirect not having arrived yet |
| Compare-and-set on `from_shard` | a handoff racing a reconnect or a second handoff |
| Atomic `GETDEL` redeem | one ticket spawning the player on two shards |
| `last_input_sequence` in the ticket | the destination re-applying already-simulated commands |

Presence transfer and ticket publication share a single round trip — the tick loop pays that latency inline, so it must not cost two.

Measured: 201,651 migrations over a 10-minute soak, 0.53 ms mean tick cost.

## Failure handling

| Failure | Response |
|---|---|
| Client drops | simulation state held for 20 s; reconnect resumes the same avatar in place |
| Client migrates | predicted state re-adopted on the new shard, so no rubber-band to origin |
| Shard dies | TTL expires, regions reassigned on next refresh, matchmaker stops routing there |
| Shard draining | connect refused with a reason; existing players handed off |
| Ticket unredeemed | TTL expires, nothing left behind |
| Redis unreachable | simulation continues; cluster operations fail soft |

A shard losing Redis keeps simulating the players it already has. Coordination degrades; the game does not stop.

## Matchmaking

Stateless. Reads the live shard set, picks the lowest `load_factor` (`players / capacity`), breaks ties by shard id so the choice is deterministic, and issues a session token. A shard at capacity or draining is not `accepting()` and is skipped.

## Observability

| Metric | Type | Answers |
|---|---|---|
| `morton_tick_seconds` | histogram | is the shard keeping its budget |
| `morton_step_seconds` | histogram | is physics the problem (it isn't) |
| `morton_replicate_seconds` | histogram | is replication the problem (it is) |
| `morton_encode_seconds` | histogram | …and is it encoding specifically |
| `morton_send_seconds` | histogram | …or the syscalls |
| `morton_migrate_seconds` | histogram | is handoff stalling the tick |
| `morton_cluster_seconds` | histogram | is Redis stalling the tick |
| `morton_snapshot_bytes` | histogram | are snapshots fitting in an MTU |
| `morton_receive_drops_total` | counter | is the kernel dropping our inputs |
| `morton_send_failures_total` | counter | is the send path failing |
| `morton_players` | gauge | residents |
| `morton_players_redirecting` | gauge | handed off but not yet gone |

Residents and redirecting players are separate gauges deliberately — counting a redirected player on the shard it is leaving *and* the one it is joining makes the fleet look larger than it is.

The phase histograms are designed to sum to the tick. When they didn't, that gap was a real coordination stall hiding outside instrumentation — which is how the presence heartbeat was found.

## Testing

16 suites, no framework, no mocks. Integration suites drive real sockets, a real Redis and real shard processes.

| Suite | Covers |
|---|---|
| `test_reliability` | ack bitfield, retransmission, ordering, loss estimation |
| `test_udp_socket` | batch send/recv, ordering, oversized datagram refusal |
| `test_connection` | handshake, salt verification, timeout, denial |
| `test_snapshot` | delta coding, baseline expiry, budget overflow |
| `test_client_view` | prediction, reconciliation, interpolation |
| `test_spatial` | Morton coding, AOI query correctness |
| `test_world` / `test_world_server` | simulation, tick pipeline |
| `test_migration` | ticket encode/decode, redemption atomicity |
| `test_matchmaker` | shard selection, load balancing |
| `test_redis` | RESP2 parsing, pipelining |
| `test_resilience` | shard loss, reconnect, migration under failure |
| `test_load` | fleet harness itself |
| `test_bitstream`, `test_metrics`, `test_websocket` | primitives |

`WorldServer::tick()` is public specifically so tests drive the shard deterministically instead of racing a background thread.
