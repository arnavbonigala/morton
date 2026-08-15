#include "proto/snapshot.h"

#include <algorithm>

#include "core/log.h"

namespace morton {
namespace {

/// Position deltas that fit in a signed 12-bit field cover a full tick of travel
/// at sprint speed plus collision separation, so nearly every moving entity
/// takes the short path instead of a 16-bit absolute coordinate.
constexpr i32 kSmallDeltaMin = -2048;
constexpr i32 kSmallDeltaMax = 2047;

constexpr u32 kVelocityBits = 12;
constexpr u32 kKindBits = 2;

/// Worst case bits for one entity record: varint id gap, flags, and a full
/// absolute state. Selection budgets against this so the writer can never
/// overflow midway through a record.
constexpr u32 kWorstCaseEntityBits = 40 + 1 + 1 + 1 + 16 + 16 + kVelocityBits * 2 + kKindBits;

constexpr u32 kUpdateCountBits = 16;

void write_entity_id(BitWriter* writer, EntityId id, EntityId previous) {
    writer->write_varint(id - previous);
}

EntityId read_entity_id(BitReader* reader, EntityId previous) {
    return previous + reader->read_varint();
}

}  // namespace

const EntityWireState* SnapshotFrame::find(EntityId id) const {
    auto it = std::lower_bound(states.begin(), states.end(), id,
                               [](const EntityWireState& state, EntityId target) {
                                   return state.id < target;
                               });
    if (it == states.end() || it->id != id) return nullptr;
    return &*it;
}

void ReplicationState::configure(const InterestConfig& config, const WorldParams& params) {
    config_ = config;
    params_ = params;
    quantizers_ = ReplicationQuantizers::from(params);
    history_.configure(config.baseline_history);
}

void ReplicationState::reset() {
    history_.reset();
    last_acked_tick_ = 0;
    has_acked_ = false;
    priority_.clear();
    stats_ = ReplicationStats{};
}

SnapshotFrame* ReplicationState::frame_for(Tick tick) { return history_.slot(tick); }

const SnapshotFrame* ReplicationState::baseline() const {
    return has_acked_ ? history_.get(last_acked_tick_) : nullptr;
}

void ReplicationState::acknowledge(Tick tick) {
    if (has_acked_ && !sequence_greater(tick, last_acked_tick_)) return;
    last_acked_tick_ = tick;
    has_acked_ = true;
}

u32 ReplicationState::encode(const World& world, EntityId viewer, u32 last_input_sequence,
                             u8* out, u32 capacity) {
    stats_ = ReplicationStats{};

    const EntityStore& entities = world.entities();
    i64 viewer_index = entities.find(viewer);
    if (viewer_index < 0) return 0;

    const Vec2 eye = entities.position[static_cast<u32>(viewer_index)];
    const f32 aoi_sq = config_.aoi_radius * config_.aoi_radius;

    // Ordering by id happens on packed id:index keys rather than on wire states,
    // so the sort moves 8 bytes per element and quantization runs once, in order.
    gather_.clear();
    world.grid().query_radius(eye, config_.aoi_radius, [&](u32 index) {
        if (distance_sq(entities.position[index], eye) > aoi_sq) return;
        gather_.push_back((static_cast<u64>(entities.id[index]) << 32) | index);
    });
    std::sort(gather_.begin(), gather_.end());

    scratch_current_.resize(gather_.size());
    for (std::size_t i = 0; i < gather_.size(); ++i) {
        u32 index = static_cast<u32>(gather_[i] & 0xffffffffull);
        EntityWireState& state = scratch_current_[i];
        state.id = static_cast<EntityId>(gather_[i] >> 32);
        state.px = static_cast<u16>(quantizers_.position.encode(entities.position[index].x));
        state.py = static_cast<u16>(quantizers_.position.encode(entities.position[index].y));
        state.vx = static_cast<u16>(quantizers_.velocity.encode(entities.velocity[index].x));
        state.vy = static_cast<u16>(quantizers_.velocity.encode(entities.velocity[index].y));
        state.kind = entities.kind[index];
    }
    stats_.relevant_entities = static_cast<u32>(scratch_current_.size());

    // A baseline old enough to share a ring slot with the frame about to be
    // written would be cleared out from under the encoder, so it is not usable.
    const SnapshotFrame* base = baseline();
    if (base != nullptr && base == frame_for(world.tick())) base = nullptr;
    const bool full = base == nullptr;
    stats_.was_full_snapshot = full;

    // Both sides are sorted by id, so one merge yields every entity's baseline
    // state and the removal list at once.
    previous_.assign(scratch_current_.size(), nullptr);
    removed_.clear();
    if (!full) {
        std::size_t b = 0;
        std::size_t c = 0;
        while (b < base->states.size() && c < scratch_current_.size()) {
            EntityId old_id = base->states[b].id;
            EntityId new_id = scratch_current_[c].id;
            if (old_id < new_id) {
                removed_.push_back(old_id);
                ++b;
            } else if (old_id > new_id) {
                ++c;
            } else {
                previous_[c] = &base->states[b];
                ++b;
                ++c;
            }
        }
        for (; b < base->states.size(); ++b) removed_.push_back(base->states[b].id);
    }

    candidates_.clear();
    candidates_.reserve(scratch_current_.size());

    for (u32 i = 0; i < scratch_current_.size(); ++i) {
        const EntityWireState& current = scratch_current_[i];
        const EntityWireState* previous = previous_[i];
        if (previous != nullptr && current.same_state_as(*previous)) continue;

        Vec2 decoded(quantizers_.position.decode(current.px),
                     quantizers_.position.decode(current.py));
        f32 closeness = 1.f - clampf(std::sqrt(distance_sq(decoded, eye)) / config_.aoi_radius,
                                     0.f, 1.f);

        auto carried = priority_.find(current.id);
        f32 priority = (carried != priority_.end() ? carried->second : 0.f) +
                       closeness * config_.distance_priority_falloff + 0.01f;
        if (current.id == viewer) priority += 1e6f;

        candidates_.push_back({i, priority});
    }

    const u32 budget = std::min(capacity, config_.max_snapshot_bytes);
    BitWriter writer(out, budget);

    writer.write_u32(world.tick());
    writer.write_bool(full);
    if (!full) writer.write_u32(base->tick);
    writer.write_u32(last_input_sequence);
    writer.write_varint(viewer);

    writer.write_varint(static_cast<u32>(removed_.size()));
    EntityId previous_id = 0;
    for (EntityId id : removed_) {
        write_entity_id(&writer, id, previous_id);
        previous_id = id;
    }
    stats_.entities_removed = static_cast<u32>(removed_.size());

    if (writer.overflowed()) {
        MORTON_LOG_WARN("snapshot header overflowed for viewer %u", viewer);
        return 0;
    }

    selected_.clear();
    u32 available_bits = writer.bits_remaining();
    u32 fits = available_bits <= kUpdateCountBits
                   ? 0
                   : (available_bits - kUpdateCountBits) / kWorstCaseEntityBits;

    // Uncontended packets are the common case, and there the priority order does
    // not matter: candidates already arrive in id order, so both sorts are skipped.
    if (candidates_.size() <= fits) {
        for (const Candidate& candidate : candidates_) selected_.push_back(candidate.index);
    } else {
        std::nth_element(candidates_.begin(), candidates_.begin() + fits, candidates_.end(),
                         [](const Candidate& a, const Candidate& b) {
                             return a.priority > b.priority;
                         });
        for (u32 i = 0; i < fits; ++i) selected_.push_back(candidates_[i].index);
        std::sort(selected_.begin(), selected_.end());
    }

    writer.write_u16(static_cast<u16>(selected_.size()));

    previous_id = 0;
    for (u32 index : selected_) {
        const EntityWireState& current = scratch_current_[index];
        const EntityWireState* previous = previous_[index];

        write_entity_id(&writer, current.id, previous_id);
        previous_id = current.id;

        bool is_new = previous == nullptr;
        writer.write_bool(is_new);

        if (is_new) {
            writer.write_u16(current.px);
            writer.write_u16(current.py);
            writer.write_bits(current.vx, kVelocityBits);
            writer.write_bits(current.vy, kVelocityBits);
            writer.write_bits(current.kind, kKindBits);
            continue;
        }

        bool position_changed = current.px != previous->px || current.py != previous->py;
        writer.write_bool(position_changed);
        if (position_changed) {
            i32 dx = static_cast<i32>(current.px) - static_cast<i32>(previous->px);
            i32 dy = static_cast<i32>(current.py) - static_cast<i32>(previous->py);
            bool small = dx >= kSmallDeltaMin && dx <= kSmallDeltaMax && dy >= kSmallDeltaMin &&
                         dy <= kSmallDeltaMax;
            writer.write_bool(small);
            if (small) {
                writer.write_ranged(dx, kSmallDeltaMin, kSmallDeltaMax);
                writer.write_ranged(dy, kSmallDeltaMin, kSmallDeltaMax);
            } else {
                writer.write_u16(current.px);
                writer.write_u16(current.py);
            }
        }

        bool velocity_changed = current.vx != previous->vx || current.vy != previous->vy;
        writer.write_bool(velocity_changed);
        if (velocity_changed) {
            writer.write_bits(current.vx, kVelocityBits);
            writer.write_bits(current.vy, kVelocityBits);
        }
    }

    if (writer.overflowed()) {
        MORTON_LOG_WARN("snapshot encode overflowed for viewer %u", viewer);
        return 0;
    }

    stats_.entities_sent = static_cast<u32>(selected_.size());
    stats_.entities_deferred = static_cast<u32>(candidates_.size() - selected_.size());
    stats_.snapshot_bytes = writer.bytes_written();

    sent_flags_.assign(scratch_current_.size(), false);
    for (u32 index : selected_) sent_flags_[index] = true;

    SnapshotFrame* frame = frame_for(world.tick());
    if (frame != nullptr) {
        frame->tick = world.tick();
        frame->valid = true;
        frame->states.clear();
        frame->states.reserve(scratch_current_.size());

        for (u32 i = 0; i < scratch_current_.size(); ++i) {
            if (sent_flags_[i]) {
                frame->states.push_back(scratch_current_[i]);
                continue;
            }
            if (previous_[i] != nullptr) frame->states.push_back(*previous_[i]);
        }
    }

    priority_.clear();
    for (const Candidate& candidate : candidates_) {
        if (sent_flags_[candidate.index]) continue;
        priority_[scratch_current_[candidate.index].id] = candidate.priority;
    }

    return stats_.snapshot_bytes;
}

bool peek_snapshot_header(const u8* data, u32 size, Tick* out_tick, Tick* out_baseline_tick,
                          bool* out_full) {
    BitReader reader(data, size);
    Tick tick = reader.read_u32();
    bool full = reader.read_bool();
    Tick baseline_tick = full ? 0 : reader.read_u32();
    if (reader.overflowed()) return false;

    *out_tick = tick;
    *out_baseline_tick = baseline_tick;
    *out_full = full;
    return true;
}

bool decode_snapshot(const u8* data, u32 size, const ReplicationQuantizers& quantizers,
                     const SnapshotFrame* baseline, DecodedSnapshot* out) {
    BitReader reader(data, size);

    out->tick = reader.read_u32();
    out->full = reader.read_bool();
    out->baseline_tick = out->full ? 0 : reader.read_u32();
    out->last_input_sequence = reader.read_u32();
    out->viewer = reader.read_varint();
    if (reader.overflowed()) return false;

    if (!out->full &&
        (baseline == nullptr || !baseline->valid || baseline->tick != out->baseline_tick)) {
        return false;
    }

    out->removed.clear();
    u32 removed_count = reader.read_varint();
    if (reader.overflowed() || removed_count > 65535) return false;

    EntityId previous_id = 0;
    for (u32 i = 0; i < removed_count; ++i) {
        previous_id = read_entity_id(&reader, previous_id);
        out->removed.push_back(previous_id);
    }
    if (reader.overflowed()) return false;

    out->states.clear();
    if (!out->full) {
        for (const EntityWireState& state : baseline->states) {
            if (std::find(out->removed.begin(), out->removed.end(), state.id) ==
                out->removed.end()) {
                out->states.push_back(state);
            }
        }
    }

    u32 updated_count = reader.read_u16();
    if (reader.overflowed()) return false;

    previous_id = 0;
    for (u32 i = 0; i < updated_count; ++i) {
        previous_id = read_entity_id(&reader, previous_id);
        EntityId id = previous_id;
        bool is_new = reader.read_bool();
        if (reader.overflowed()) return false;

        auto existing = std::lower_bound(
            out->states.begin(), out->states.end(), id,
            [](const EntityWireState& state, EntityId target) { return state.id < target; });
        bool had_previous = existing != out->states.end() && existing->id == id;

        EntityWireState state;
        state.id = id;
        if (had_previous) state = *existing;

        if (is_new) {
            state.px = reader.read_u16();
            state.py = reader.read_u16();
            state.vx = static_cast<u16>(reader.read_bits(kVelocityBits));
            state.vy = static_cast<u16>(reader.read_bits(kVelocityBits));
            state.kind = static_cast<u8>(reader.read_bits(kKindBits));
        } else {
            if (!had_previous) return false;
            if (reader.read_bool()) {
                if (reader.read_bool()) {
                    state.px = static_cast<u16>(static_cast<i32>(state.px) +
                                                reader.read_ranged(kSmallDeltaMin, kSmallDeltaMax));
                    state.py = static_cast<u16>(static_cast<i32>(state.py) +
                                                reader.read_ranged(kSmallDeltaMin, kSmallDeltaMax));
                } else {
                    state.px = reader.read_u16();
                    state.py = reader.read_u16();
                }
            }
            if (reader.read_bool()) {
                state.vx = static_cast<u16>(reader.read_bits(kVelocityBits));
                state.vy = static_cast<u16>(reader.read_bits(kVelocityBits));
            }
        }
        if (reader.overflowed()) return false;

        if (had_previous) {
            *existing = state;
        } else {
            out->states.insert(existing, state);
        }
    }

    return true;
}

}  // namespace morton
