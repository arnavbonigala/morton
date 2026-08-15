#pragma once
#include <deque>
#include <vector>

#include "core/types.h"
#include "proto/snapshot.h"
#include "sim/movement.h"

namespace morton {

struct ClientViewConfig {
    /// Render remote entities this far behind the newest snapshot so there is
    /// always a later sample to interpolate toward. Two ticks at 30Hz is 66ms,
    /// enough to ride out a single dropped snapshot without extrapolating.
    f32 interpolation_delay_ticks = 2.f;

    /// Prediction errors are absorbed over time rather than snapped, so a
    /// collision the client could not predict does not teleport the avatar.
    f32 correction_smoothing = 0.25f;

    /// Beyond this the error is too large to hide, so the view snaps instead of
    /// sliding the avatar across the map.
    f32 correction_snap_distance = 120.f;

    u32 input_history = 128;
    u32 snapshot_history = 32;
};

struct RenderEntity {
    EntityId id = 0;
    Vec2 position;
    Vec2 velocity;
    u8 kind = 0;
    bool is_local = false;
};

struct ClientViewStats {
    u32 snapshots_applied = 0;
    u32 snapshots_rejected = 0;
    u32 reconciliations = 0;
    u32 replayed_inputs = 0;
    u32 hard_snaps = 0;
    f32 last_prediction_error = 0.f;
    f32 peak_prediction_error = 0.f;
    f32 mean_prediction_error = 0.f;
    u32 known_entities = 0;
    u32 pending_inputs = 0;
};

/// Client-side mirror of the world: predicts the local player forward from the
/// last authoritative state, reconciles when the server disagrees, and
/// interpolates every other entity between snapshots.
class ClientView {
public:
    void configure(const WorldParams& params, const ClientViewConfig& config);
    void reset();

    void set_local_entity(EntityId entity) { local_entity_ = entity; }
    EntityId local_entity() const { return local_entity_; }

    /// Builds the next command, applies it to the predicted state immediately,
    /// and records it for replay. The returned input is already quantized, so
    /// what is predicted is exactly what the server will integrate.
    MoveInput push_input(const Vec2& move_axis, bool sprint, Tick client_tick);

    /// Decodes a snapshot and reconciles prediction against it.
    bool apply_snapshot(const u8* data, u32 size);

    /// Advances interpolation. `dt_seconds` is real elapsed time, not tick time.
    void advance(f32 dt_seconds);

    /// Local player position including the residual correction offset.
    Vec2 render_position() const { return predicted_.position + correction_offset_; }
    Vec2 predicted_position() const { return predicted_.position; }
    Vec2 authoritative_position() const { return authoritative_.position; }

    const std::vector<RenderEntity>& render_entities() const { return render_; }
    const ClientViewStats& stats() const { return stats_; }
    Tick latest_snapshot_tick() const { return latest_tick_; }
    bool has_snapshot() const { return has_snapshot_; }
    u32 last_acknowledged_input() const { return last_acknowledged_input_; }
    u32 next_input_sequence() const { return next_input_sequence_; }

    /// Reinstates predicted state after a shard migration, so the avatar does
    /// not rubber-band back to the origin while the new shard's first snapshot
    /// is in flight.
    void adopt_migrated_state(EntityId entity, const Vec2& position, const Vec2& velocity);

private:
    struct InterpolationFrame {
        Tick tick = 0;
        std::vector<EntityWireState> states;
    };

    void reconcile(const DecodedSnapshot& snapshot);
    void rebuild_render_list();
    const InterpolationFrame* frame_at_or_before(f32 tick) const;
    const InterpolationFrame* frame_after(f32 tick) const;

    WorldParams params_;
    ClientViewConfig config_;
    ReplicationQuantizers quantizers_;

    EntityId local_entity_ = kInvalidEntity;
    MoveState predicted_;
    MoveState authoritative_;
    Vec2 correction_offset_;

    std::deque<MoveInput> input_history_;
    u32 next_input_sequence_ = 1;
    u32 last_acknowledged_input_ = 0;

    SnapshotHistory baselines_;
    std::deque<InterpolationFrame> frames_;
    Tick latest_tick_ = 0;
    bool has_snapshot_ = false;

    f32 render_tick_ = 0.f;
    bool render_clock_started_ = false;

    std::vector<RenderEntity> render_;
    ClientViewStats stats_;
    f64 error_sum_ = 0.0;
    u32 error_samples_ = 0;
};

}  // namespace morton
