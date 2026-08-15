# Protocol

Morton speaks a custom binary protocol over UDP. Everything is bit-packed — fields are not byte-aligned unless the layout happens to make them so.

| | |
|---|---|
| Protocol id | `0x4D525401` ("MRT" + version) |
| Transport | UDP, one datagram per packet |
| Byte order | little-endian |
| Max snapshot | 1000 bytes (one MTU) |
| Max reliable message | 512 bytes |
| Ack window | 1024 packets |

---

## Packet frame

Every datagram, in every state:

```
┌──────────────┬──────────┬──────────────────────────────────┐
│ protocol id  │  type    │            body                  │
│   u32        │   u8     │      (type-dependent)            │
└──────────────┴──────────┴──────────────────────────────────┘
```

A datagram whose protocol id doesn't match is dropped before anything else is read — stray traffic never reaches the parser.

| `type` | Packet |
|---:|---|
| 1 | connection request |
| 2 | challenge |
| 3 | challenge response |
| 4 | connection accepted |
| 5 | connection denied |
| 6 | payload |
| 7 | disconnect |

## Handshake

Three ways, salted, with the request padded to defeat amplification:

```
client                                                    shard
  │                                                         │
  │─── request:  client_salt(u64) token(32B) pad(1000B) ───▶│
  │                                                         │  no state allocated
  │◀── challenge: client_salt(u64) server_salt(u64) ────────│
  │                                                         │
  │─── response: client_salt ^ server_salt (u64) ──────────▶│
  │                                                         │  verify xor, admit
  │◀── accepted: salt(u64) client_id(u32) entity(u32) ──────│
  │                                                         │
  │═══ payload: salt(u64) + reliability body ══════════════▶│
```

Three properties fall out of this shape:

| Property | Mechanism |
|---|---|
| No amplification | The request is padded to ≥1040 bytes, so the challenge is smaller than what provoked it |
| No spoofed-source state | The shard allocates nothing until the response proves the client received the challenge |
| Cheap per-packet auth | The xor-salt is echoed on every payload packet and checked before parsing |

A rejected connect returns a reason: server full, bad token, wrong region, or shard draining.

### Credentials

The 32-byte token carries one of two credentials, and they are not interchangeable:

| Kind | Proves | Issued by |
|---|---|---|
| Session | the matchmaker placed this player here | matchmaker |
| Migration | another shard handed this player over | source shard |

Single-purpose by construction — a stolen migration ticket cannot be replayed as a login.

## Reliability layer

Inside every payload packet, after the salt:

```
┌─────────┬──────────┬───────────┬───────┬═══════════════┬────────┬═══════════┐
│ seq u16 │ ack  u16 │ ack_bits  │ n  u8 │  n × reliable │ len    │ unreliable│
│         │          │   u32     │       │    message    │  u16   │  payload  │
└─────────┴──────────┴───────────┴───────┴═══════════════┴────────┴═══════════┘
                                              ↓
                                  ┌────────┬─────────┬──────┬─────────┐
                                  │ id u16 │ chan u8 │ len  │ payload │
                                  └────────┴─────────┴──────┴─────────┘
```

`ack` is the newest sequence seen from the peer; `ack_bits` is a bitfield of the 32 before it. **One received packet acknowledges up to 33** — so acks survive loss without a dedicated ack packet or timer.

Reliable messages piggyback on that ack stream. They ride in ordinary packets and are retransmitted until a packet carrying them is acked, so there is no per-message retransmit timer anywhere in the system. Up to 8 messages per packet, delivered in order, deduplicated by id.

**Loss estimation.** A packet unacked 32 sequences past the cursor is counted lost — a full second at 30 Hz, far beyond plausible reordering. RTT, jitter and rates are EWMA-smoothed over a 250 ms window.

## Channels

| Channel | Direction | Delivery |
|---|---|---|
| 1 — input | client → shard | unreliable, redundant |
| 2 — snapshot | shard → client | unreliable, delta-coded |
| 3 — event | both | reliable, ordered |

### Events

| id | Event | Carries |
|---:|---|---|
| 1 | migrate redirect | ticket u64, region, endpoint, shard id |
| 2 | migrate complete | — |
| 3 | world config | entity, tick, region, world size, tick rate, shard id |
| 4 | player joined | — |
| 5 | player left | — |
| 6 | shard draining | — |

## Input packets

```
channel(u8=1) │ client_tick u32 │ acked_snapshot_tick u32 │ count(ranged 0..16)
              └─▶ per input: sequence u32 │ move_x │ move_y │ sprint(1 bit)
```

Move axes are quantized before they leave the client, so **what the client predicts is bit-for-bit what the server integrates** — quantization can't be a source of divergence.

Each command is sent in **6 consecutive packets**. The server discards duplicates by sequence, so the redundancy costs bandwidth and nothing else — but a single dropped datagram no longer leaves a gap the simulation has to stall on.

## Snapshots

### Wire resolution

| Field | Bits | Resolution @ 2048-unit world |
|---|---:|---|
| position x, y | 16 | ~0.03 units |
| velocity x, y | 12 | ±2× max speed |
| kind | 2 | 4 entity types |

Position at 16 bits is half the size of a float and far below what a player can perceive.

### Frame layout

```
tick u32 │ full(1 bit) │ [baseline_tick u32 if delta] │ last_input_seq u32 │ viewer varint
─────────────────────────────────────────────────────────────────────────────────────────
removed_count varint │ removed ids (varint delta-coded)
─────────────────────────────────────────────────────────────────────────────────────────
updated_count u16    │ per entity:
                     │   id (varint delta from previous id)
                     │   is_new (1 bit)
                     │     ├─ new:  px u16 │ py u16 │ vx 12 │ vy 12 │ kind 2
                     │     └─ delta: pos_changed(1) │ small(1) │ …
                     │                vel_changed(1) │ …
```

Three compounding tricks:

| Trick | Effect |
|---|---|
| **Ids delta-coded** | Entities are sorted by id, so consecutive ids cost a varint of the gap, not 32 bits |
| **Change bits** | An entity that didn't move costs 1 bit for `pos_changed`; one that didn't move at all is skipped entirely |
| **Small deltas** | A position delta in ±2048 is written ranged instead of as two `u16`s |

Because comparison happens on the **quantized** state rather than the float state, an entity whose float position wobbled below the wire resolution registers as unchanged and costs nothing.

### Baselines

The server keeps a ring of 32 recent frames per client. The client's acked tick selects the delta baseline; a baseline old enough to share a ring slot with the frame being written is refused, so a wrapped frame can never be mistaken for a valid one. Diffing is a **linear merge** over two id-sorted lists — it yields every entity's previous state and the removal list in one pass.

## Interest management

Per viewer, per tick:

```
  spatial query           priority                 budget
  (Morton grid)              │                       │
       │                     │                       │
  entities in     ──▶   carried + closeness   ──▶  fill 1000 B  ──▶ snapshot
  260-unit AOI          + 0.01 (+1e6 self)         by priority
                             │                       │
                             └── deferred entities keep their credit ──┘
```

| Rule | Why |
|---|---|
| Outside AOI → dropped | Not relevant; never costs a bit |
| Closer → higher priority | Contended packets favour what matters visually |
| Deferred → priority carries over | An entity that loses several ticks eventually outranks closer ones — **no starvation** |
| Viewer → +1e6 | Your own avatar is never cut |

The carried-priority lookup walks one cursor over an id-sorted array rather than hashing every id.

Measured effect: at a fixed 500 viewers, taking the world from 500 to 4000 drifters (8×) moves the snapshot from 474 B to 578 B. Cost tracks what a viewer can *see*, not what exists. See [BENCHMARKS.md](BENCHMARKS.md).

## Client reconciliation

On each snapshot naming the local entity:

```
1. drop if last_input_sequence went backwards
2. adopt authoritative position and velocity
3. discard inputs the server has already applied
4. replay every remaining input against the authoritative state
5. error = |replayed − previously drawn|
6. error > 120 units ─▶ snap
   otherwise         ─▶ fold into correction_offset, retire at 25%/frame
```

Step 6 is what makes it invisible: the avatar stays where it was drawn and the difference is retired over the next few frames. A bare assignment there is exactly what produces visible rubber-banding.

Remote entities render **2 ticks (66 ms) behind** the newest snapshot, so there is always a later sample to interpolate toward — enough to ride out one dropped snapshot without extrapolating.

Measured prediction error over a 10-minute soak at 2000 clients: **1.63 mean, 9.54 p99**.

## Migration tickets

Serialized with a version tag and a checksum over the body; truncated, corrupt and wrong-version blobs are rejected rather than trusted.

| Field | Purpose |
|---|---|
| token, player id | identity |
| from shard, to shard | routing |
| region, tick | placement |
| position, velocity | resume mid-stride |
| **last input sequence** | **prevents the destination re-applying commands the source already simulated** |

That last field is the one that matters: without it the destination replays inputs the source already integrated, and the client's own replay disagrees with the server on the very first tick after handoff.

Redemption is an atomic Redis `GETDEL`. A replayed or duplicated redirect finds nothing on the second attempt, so the same player cannot be spawned onto two shards. Tickets carry a short TTL, so a client that never arrives leaves nothing behind.
