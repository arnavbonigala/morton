#pragma once
#include "core/types.h"
#include "proto/bitstream.h"

namespace morton {

/// Tuning shared by every shard and every client. Values are fixed at build time
/// because prediction only reconciles if both sides integrate identically.
struct WorldParams {
    f32 size = 2048.f;
    f32 acceleration = 1600.f;
    f32 max_speed = 320.f;
    f32 friction = 6.f;
    f32 player_radius = 8.f;
    u32 tick_rate = 30;

    f32 tick_dt() const { return 1.f / static_cast<f32>(tick_rate); }
};

/// One client command. `sequence` is what the server echoes back so the client
/// knows how much of its predicted history has been confirmed.
struct MoveInput {
    u32 sequence = 0;
    Tick tick = 0;
    f32 move_x = 0.f;
    f32 move_y = 0.f;
    bool sprint = false;
};

struct MoveState {
    Vec2 position;
    Vec2 velocity;
};

/// Input axes are quantized on the wire, so both sides must integrate the
/// quantized value. Applying the raw analog value client-side is the classic
/// source of slow prediction drift that only shows up under sustained input.
inline const Quantizer& input_axis_quantizer() {
    static const Quantizer q{-1.f, 1.f, 8};
    return q;
}

inline void quantize_input(MoveInput* input) {
    const Quantizer& q = input_axis_quantizer();
    input->move_x = q.snap(clampf(input->move_x, -1.f, 1.f));
    input->move_y = q.snap(clampf(input->move_y, -1.f, 1.f));
}

/// Advances one entity by exactly one tick. Pure, branch-stable and free of any
/// transcendental calls, so it produces bit-identical results on client and
/// server for the same inputs.
inline void step_movement(MoveState* state, const MoveInput& input, const WorldParams& params,
                          f32 dt) {
    Vec2 direction(input.move_x, input.move_y);
    f32 magnitude_sq = direction.length_sq();
    if (magnitude_sq > 1.f) direction = direction * (1.f / std::sqrt(magnitude_sq));

    f32 acceleration = params.acceleration * (input.sprint ? 1.6f : 1.f);
    f32 max_speed = params.max_speed * (input.sprint ? 1.5f : 1.f);

    state->velocity += direction * (acceleration * dt);

    f32 damping = 1.f / (1.f + params.friction * dt);
    state->velocity *= damping;
    state->velocity = state->velocity.clamped(max_speed);

    state->position += state->velocity * dt;

    f32 low = params.player_radius;
    f32 high = params.size - params.player_radius;
    if (state->position.x < low) {
        state->position.x = low;
        if (state->velocity.x < 0.f) state->velocity.x = 0.f;
    } else if (state->position.x > high) {
        state->position.x = high;
        if (state->velocity.x > 0.f) state->velocity.x = 0.f;
    }
    if (state->position.y < low) {
        state->position.y = low;
        if (state->velocity.y < 0.f) state->velocity.y = 0.f;
    } else if (state->position.y > high) {
        state->position.y = high;
        if (state->velocity.y > 0.f) state->velocity.y = 0.f;
    }
}

}  // namespace morton
