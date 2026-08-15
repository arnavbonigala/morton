#include "sim/client_view.h"

#include <algorithm>

#include "core/log.h"

namespace morton {
namespace {

/// If the render clock falls this far from where it should be, catching up
/// smoothly would take longer than the desync is worth hiding.
constexpr f32 kRenderClockSnapTicks = 10.f;

constexpr f32 kRenderClockCatchupRate = 0.08f;

const EntityWireState* find_state(const std::vector<EntityWireState>& states, EntityId id) {
    auto it = std::lower_bound(
        states.begin(), states.end(), id,
        [](const EntityWireState& state, EntityId target) { return state.id < target; });
    return (it == states.end() || it->id != id) ? nullptr : &*it;
}

}  // namespace

void ClientView::configure(const WorldParams& params, const ClientViewConfig& config) {
    params_ = params;
    config_ = config;
    quantizers_ = ReplicationQuantizers::from(params);
    baselines_.configure(config.snapshot_history);
    reset();
}

void ClientView::reset() {
    predicted_ = MoveState{};
    authoritative_ = MoveState{};
    correction_offset_ = Vec2();
    input_history_.clear();
    next_input_sequence_ = 1;
    last_acknowledged_input_ = 0;
    baselines_.reset();
    frames_.clear();
    latest_tick_ = 0;
    has_snapshot_ = false;
    render_tick_ = 0.f;
    render_clock_started_ = false;
    render_.clear();
    stats_ = ClientViewStats{};
    error_sum_ = 0.0;
    error_samples_ = 0;
}

void ClientView::adopt_migrated_state(EntityId entity, const Vec2& position,
                                      const Vec2& velocity) {
    local_entity_ = entity;
    predicted_.position = position;
    predicted_.velocity = velocity;
    authoritative_ = predicted_;
    correction_offset_ = Vec2();
    input_history_.clear();
    baselines_.reset();
    frames_.clear();
    has_snapshot_ = false;
    render_clock_started_ = false;
}

MoveInput ClientView::push_input(const Vec2& move_axis, bool sprint, Tick client_tick) {
    MoveInput input;
    input.sequence = next_input_sequence_++;
    input.tick = client_tick;
    input.move_x = move_axis.x;
    input.move_y = move_axis.y;
    input.sprint = sprint;
    quantize_input(&input);

    step_movement(&predicted_, input, params_, params_.tick_dt());

    input_history_.push_back(input);
    while (input_history_.size() > config_.input_history) input_history_.pop_front();
    stats_.pending_inputs = static_cast<u32>(input_history_.size());

    return input;
}

bool ClientView::apply_snapshot(const u8* data, u32 size) {
    Tick tick = 0;
    Tick baseline_tick = 0;
    bool full = false;
    if (!peek_snapshot_header(data, size, &tick, &baseline_tick, &full)) {
        ++stats_.snapshots_rejected;
        return false;
    }

    const SnapshotFrame* baseline = full ? nullptr : baselines_.get(baseline_tick);
    if (!full && baseline == nullptr) {
        ++stats_.snapshots_rejected;
        return false;
    }

    DecodedSnapshot snapshot;
    if (!decode_snapshot(data, size, quantizers_, baseline, &snapshot)) {
        ++stats_.snapshots_rejected;
        return false;
    }

    SnapshotFrame* frame = baselines_.slot(snapshot.tick);
    if (frame != nullptr) {
        frame->tick = snapshot.tick;
        frame->valid = true;
        frame->states = snapshot.states;
    }

    if (snapshot.viewer != kInvalidEntity) local_entity_ = snapshot.viewer;

    InterpolationFrame interpolation;
    interpolation.tick = snapshot.tick;
    interpolation.states = snapshot.states;

    auto position = std::lower_bound(
        frames_.begin(), frames_.end(), snapshot.tick,
        [](const InterpolationFrame& frame, Tick target) { return frame.tick < target; });
    if (position != frames_.end() && position->tick == snapshot.tick) {
        *position = std::move(interpolation);
    } else {
        frames_.insert(position, std::move(interpolation));
    }
    while (frames_.size() > config_.snapshot_history) frames_.pop_front();

    if (!has_snapshot_ || sequence_greater(snapshot.tick, latest_tick_)) {
        latest_tick_ = snapshot.tick;
    }
    has_snapshot_ = true;
    ++stats_.snapshots_applied;
    stats_.known_entities = static_cast<u32>(snapshot.states.size());

    reconcile(snapshot);
    return true;
}

void ClientView::reconcile(const DecodedSnapshot& snapshot) {
    const EntityWireState* mine = nullptr;
    for (const EntityWireState& state : snapshot.states) {
        if (state.id == local_entity_) {
            mine = &state;
            break;
        }
    }
    if (mine == nullptr) return;

    if (snapshot.last_input_sequence < last_acknowledged_input_) return;
    last_acknowledged_input_ = snapshot.last_input_sequence;

    authoritative_.position =
        Vec2(quantizers_.position.decode(mine->px), quantizers_.position.decode(mine->py));
    authoritative_.velocity =
        Vec2(quantizers_.velocity.decode(mine->vx), quantizers_.velocity.decode(mine->vy));

    while (!input_history_.empty() &&
           input_history_.front().sequence <= snapshot.last_input_sequence) {
        input_history_.pop_front();
    }

    Vec2 previous_prediction = predicted_.position;

    MoveState replayed = authoritative_;
    for (const MoveInput& input : input_history_) {
        step_movement(&replayed, input, params_, params_.tick_dt());
        ++stats_.replayed_inputs;
    }

    f32 error = (replayed.position - previous_prediction).length();
    stats_.last_prediction_error = error;
    if (error > stats_.peak_prediction_error) stats_.peak_prediction_error = error;
    error_sum_ += error;
    ++error_samples_;
    stats_.mean_prediction_error = static_cast<f32>(error_sum_ / error_samples_);

    if (error > 0.f) ++stats_.reconciliations;

    // Keep the avatar where it was drawn and retire the difference over the next
    // few frames; a bare assignment here is what produces visible rubber-banding.
    if (error > config_.correction_snap_distance) {
        correction_offset_ = Vec2();
        ++stats_.hard_snaps;
    } else {
        correction_offset_ = previous_prediction - replayed.position + correction_offset_;
    }

    predicted_ = replayed;
    stats_.pending_inputs = static_cast<u32>(input_history_.size());
}

const ClientView::InterpolationFrame* ClientView::frame_at_or_before(f32 tick) const {
    const InterpolationFrame* best = nullptr;
    for (const InterpolationFrame& frame : frames_) {
        if (static_cast<f32>(frame.tick) <= tick) best = &frame;
        else break;
    }
    return best;
}

const ClientView::InterpolationFrame* ClientView::frame_after(f32 tick) const {
    for (const InterpolationFrame& frame : frames_) {
        if (static_cast<f32>(frame.tick) > tick) return &frame;
    }
    return nullptr;
}

void ClientView::advance(f32 dt_seconds) {
    correction_offset_ *= (1.f - clampf(config_.correction_smoothing, 0.f, 1.f));
    if (correction_offset_.length_sq() < 0.0001f) correction_offset_ = Vec2();

    if (!has_snapshot_) {
        rebuild_render_list();
        return;
    }

    f32 target = static_cast<f32>(latest_tick_) - config_.interpolation_delay_ticks;
    if (!render_clock_started_) {
        render_tick_ = target;
        render_clock_started_ = true;
    } else {
        render_tick_ += dt_seconds * static_cast<f32>(params_.tick_rate);
        f32 drift = target - render_tick_;
        if (std::fabs(drift) > kRenderClockSnapTicks) {
            render_tick_ = target;
        } else {
            render_tick_ += drift * kRenderClockCatchupRate;
        }
    }

    rebuild_render_list();
}

void ClientView::rebuild_render_list() {
    render_.clear();

    const InterpolationFrame* older = frame_at_or_before(render_tick_);
    const InterpolationFrame* newer = frame_after(render_tick_);

    if (older == nullptr && newer == nullptr) {
        if (local_entity_ != kInvalidEntity) {
            RenderEntity local;
            local.id = local_entity_;
            local.position = render_position();
            local.velocity = predicted_.velocity;
            local.is_local = true;
            render_.push_back(local);
        }
        return;
    }
    if (older == nullptr) older = newer;
    if (newer == nullptr) newer = older;

    f32 span = static_cast<f32>(newer->tick) - static_cast<f32>(older->tick);
    f32 alpha = span > 0.f ? clampf((render_tick_ - static_cast<f32>(older->tick)) / span, 0.f, 1.f)
                           : 0.f;

    for (const EntityWireState& state : older->states) {
        RenderEntity entity;
        entity.id = state.id;
        entity.kind = state.kind;
        entity.is_local = state.id == local_entity_;

        Vec2 from(quantizers_.position.decode(state.px), quantizers_.position.decode(state.py));
        Vec2 from_velocity(quantizers_.velocity.decode(state.vx),
                           quantizers_.velocity.decode(state.vy));

        const EntityWireState* future = find_state(newer->states, state.id);
        if (future != nullptr) {
            Vec2 to(quantizers_.position.decode(future->px),
                    quantizers_.position.decode(future->py));
            Vec2 to_velocity(quantizers_.velocity.decode(future->vx),
                             quantizers_.velocity.decode(future->vy));
            entity.position = lerp(from, to, alpha);
            entity.velocity = lerp(from_velocity, to_velocity, alpha);
        } else {
            entity.position = from;
            entity.velocity = from_velocity;
        }

        if (entity.is_local) {
            entity.position = render_position();
            entity.velocity = predicted_.velocity;
        }
        render_.push_back(entity);
    }

    bool has_local = false;
    for (const RenderEntity& entity : render_) {
        if (entity.is_local) {
            has_local = true;
            break;
        }
    }
    if (!has_local && local_entity_ != kInvalidEntity) {
        RenderEntity local;
        local.id = local_entity_;
        local.position = render_position();
        local.velocity = predicted_.velocity;
        local.is_local = true;
        render_.push_back(local);
    }
}

}  // namespace morton
