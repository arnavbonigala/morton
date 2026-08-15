#pragma once
#include <array>
#include <unordered_map>
#include <vector>

#include "core/types.h"
#include "proto/bitstream.h"
#include "sim/movement.h"
#include "sim/world.h"

namespace morton {

/// Wire resolution for replicated state. Position uses 16 bits across the world
/// axis, which is ~0.03 units at a 2048-unit world: far below what a player can
/// perceive, and half the size of a float.
struct ReplicationQuantizers {
    Quantizer position;
    Quantizer velocity;

    static ReplicationQuantizers from(const WorldParams& params) {
        ReplicationQuantizers q;
        q.position = Quantizer{0.f, params.size, 16};
        q.velocity = Quantizer{-params.max_speed * 2.f, params.max_speed * 2.f, 12};
        return q;
    }
};

/// One entity as it exists on the wire, already quantized. Comparing these
/// rather than the float state is what lets the encoder skip entities whose
/// visible state did not actually change.
struct EntityWireState {
    EntityId id = 0;
    u16 px = 0;
    u16 py = 0;
    u16 vx = 0;
    u16 vy = 0;
    u8 kind = 0;

    bool same_state_as(const EntityWireState& o) const {
        return px == o.px && py == o.py && vx == o.vx && vy == o.vy && kind == o.kind;
    }
};

/// A snapshot as sent, kept so a later ack can be used as a delta baseline.
/// Entities are stored sorted by id so diffing two frames is a linear merge.
struct SnapshotFrame {
    Tick tick = 0;
    bool valid = false;
    std::vector<EntityWireState> states;

    const EntityWireState* find(EntityId id) const;
    void clear() {
        tick = 0;
        valid = false;
        states.clear();
    }
};

/// Ring of recent frames keyed by tick, used by the server to hold candidate
/// baselines and by the client to hold the frames it can decode against.
class SnapshotHistory {
public:
    void configure(u32 capacity) { frames_.assign(capacity, SnapshotFrame{}); }

    SnapshotFrame* slot(Tick tick) {
        return frames_.empty() ? nullptr : &frames_[tick % frames_.size()];
    }

    /// Returns the frame for `tick` only if that exact tick still occupies the
    /// slot, so a wrapped-over frame is never mistaken for a valid baseline.
    const SnapshotFrame* get(Tick tick) const {
        if (frames_.empty()) return nullptr;
        const SnapshotFrame& frame = frames_[tick % frames_.size()];
        if (!frame.valid || frame.tick != tick) return nullptr;
        return &frame;
    }

    void reset() {
        for (SnapshotFrame& frame : frames_) frame.clear();
    }

    u32 capacity() const { return static_cast<u32>(frames_.size()); }

private:
    std::vector<SnapshotFrame> frames_;
};

struct InterestConfig {
    f32 aoi_radius = 260.f;
    u32 max_snapshot_bytes = 1000;
    u32 baseline_history = 32;
    /// Entities beyond the AOI are dropped outright; inside it, closer entities
    /// win contention for a full packet.
    f32 distance_priority_falloff = 1.f;
};

struct ReplicationStats {
    u32 relevant_entities = 0;
    u32 entities_sent = 0;
    u32 entities_removed = 0;
    u32 entities_deferred = 0;
    u32 snapshot_bytes = 0;
    bool was_full_snapshot = false;
};

/// Per-client replication state: the acked baseline history and the priority
/// accumulator that decides who gets into a contended packet.
///
/// Deferred entities keep accumulating priority, so an entity that loses the
/// race for several ticks eventually outranks closer ones and cannot be starved
/// indefinitely.
class ReplicationState {
public:
    void configure(const InterestConfig& config, const WorldParams& params);

    /// Builds and encodes a snapshot for `viewer`. Returns bytes written.
    u32 encode(const World& world, EntityId viewer, u32 last_input_sequence, u8* out,
               u32 capacity);

    /// Records that the client received the snapshot for `tick`, making it the
    /// new delta baseline.
    void acknowledge(Tick tick);

    const ReplicationStats& stats() const { return stats_; }
    Tick last_acked_tick() const { return last_acked_tick_; }
    bool has_baseline() const { return has_acked_; }
    void reset();

private:
    struct Candidate {
        u32 index;
        f32 priority;
    };

    SnapshotFrame* frame_for(Tick tick);
    const SnapshotFrame* baseline() const;

    InterestConfig config_;
    ReplicationQuantizers quantizers_;
    WorldParams params_;

    SnapshotHistory history_;
    Tick last_acked_tick_ = 0;
    bool has_acked_ = false;

    std::unordered_map<EntityId, f32> priority_;
    std::vector<u64> gather_;
    std::vector<EntityWireState> scratch_current_;
    std::vector<const EntityWireState*> previous_;
    std::vector<Candidate> candidates_;
    std::vector<u32> selected_;
    std::vector<EntityId> removed_;
    std::vector<bool> sent_flags_;
    ReplicationStats stats_;
};

/// Decoded snapshot, as reconstructed by a client against its own baseline.
struct DecodedSnapshot {
    Tick tick = 0;
    Tick baseline_tick = 0;
    bool full = false;
    u32 last_input_sequence = 0;
    EntityId viewer = kInvalidEntity;
    std::vector<EntityWireState> states;
    std::vector<EntityId> removed;
};

/// Applies a delta snapshot to `baseline`, producing the full entity set for the
/// snapshot's tick. Returns false if the packet is malformed or references a
/// baseline the caller does not hold.
bool decode_snapshot(const u8* data, u32 size, const ReplicationQuantizers& quantizers,
                     const SnapshotFrame* baseline, DecodedSnapshot* out);

/// Reads only the header, so a client can find which baseline a packet needs
/// before committing to decode it.
bool peek_snapshot_header(const u8* data, u32 size, Tick* out_tick, Tick* out_baseline_tick,
                          bool* out_full);

}  // namespace morton
