#include <deque>
#include <random>
#include <vector>

#include "sim/client_view.h"
#include "sim/world.h"
#include "tests/check.h"

using namespace morton;

namespace {

WorldParams test_params() {
    WorldParams params;
    params.size = 2048.f;
    params.tick_rate = 30;
    return params;
}

/// Server and one predicting client joined by a delay pipeline, so prediction
/// and reconciliation are exercised the way they behave over a real link.
struct Loop {
    World world;
    ReplicationState replication;
    ClientView view;
    EntityId player = kInvalidEntity;
    ClientId client_id = 1;

    u32 latency_ticks = 3;
    f32 loss = 0.f;
    std::mt19937 rng{1234};

    std::deque<std::pair<u32, MoveInput>> to_server;
    std::deque<std::pair<u32, std::vector<u8>>> to_client;
    u32 now = 0;

    void build(const Vec2& start, u32 drifters = 0) {
        WorldParams params = test_params();
        world.configure(params, 32);

        InterestConfig interest;
        replication.configure(interest, params);

        ClientViewConfig view_config;
        view.configure(params, view_config);

        player = world.spawn_player(client_id, start);
        view.set_local_entity(player);
        view.adopt_migrated_state(player, start, Vec2());

        std::mt19937 spawn_rng(7);
        std::uniform_real_distribution<f32> coord(20.f, params.size - 20.f);
        for (u32 i = 0; i < drifters; ++i) world.spawn_drifter(Vec2(coord(spawn_rng), coord(spawn_rng)));

        world.step();
    }

    bool dropped() { return std::uniform_real_distribution<f32>(0.f, 1.f)(rng) < loss; }

    void tick(const Vec2& axis) {
        MoveInput input = view.push_input(axis, false, now);
        if (!dropped()) to_server.push_back({now + latency_ticks, input});

        while (!to_server.empty() && to_server.front().first <= now) {
            world.queue_input(client_id, to_server.front().second);
            to_server.pop_front();
        }

        world.step();

        const PlayerSlot* slot = world.player_of(client_id);
        u32 acknowledged = slot != nullptr ? slot->last_applied_sequence : 0;

        u8 packet[1200];
        u32 size = replication.encode(world, player, acknowledged, packet, sizeof(packet));
        if (size > 0 && !dropped()) {
            to_client.push_back({now + latency_ticks, std::vector<u8>(packet, packet + size)});
        }

        while (!to_client.empty() && to_client.front().first <= now) {
            const std::vector<u8>& bytes = to_client.front().second;
            if (view.apply_snapshot(bytes.data(), static_cast<u32>(bytes.size()))) {
                replication.acknowledge(view.latest_snapshot_tick());
            }
            to_client.pop_front();
        }

        view.advance(world.params().tick_dt());
        ++now;
    }

    Vec2 server_position() const {
        i64 index = world.entities().find(player);
        return index < 0 ? Vec2() : world.entities().position[static_cast<u32>(index)];
    }
};

}  // namespace

TEST_CASE(prediction_stays_ahead_of_the_server_by_the_link_delay) {
    Loop loop;
    loop.latency_ticks = 4;
    loop.build(Vec2(400.f, 400.f));

    for (u32 i = 0; i < 60; ++i) loop.tick(Vec2(1.f, 0.f));

    Vec2 predicted = loop.view.predicted_position();
    Vec2 authoritative = loop.server_position();

    CHECK(predicted.x > authoritative.x);
    CHECK(predicted.x - authoritative.x < 120.f);
    CHECK(loop.view.stats().snapshots_applied > 40u);
}

TEST_CASE(reconciliation_keeps_prediction_error_near_zero_without_obstacles) {
    Loop loop;
    loop.latency_ticks = 5;
    loop.build(Vec2(300.f, 300.f));

    std::mt19937 rng(99);
    std::uniform_real_distribution<f32> axis(-1.f, 1.f);

    for (u32 i = 0; i < 400; ++i) loop.tick(Vec2(axis(rng), axis(rng)));

    const ClientViewStats& stats = loop.view.stats();
    std::printf("       mean error=%.4f peak=%.4f snaps=%u\n", stats.mean_prediction_error,
                stats.peak_prediction_error, stats.hard_snaps);

    CHECK(stats.mean_prediction_error < 0.5f);
    CHECK(stats.hard_snaps == 0u);
    CHECK(stats.snapshots_rejected == 0u);
}

TEST_CASE(client_recovers_from_a_deliberately_corrupted_prediction) {
    Loop loop;
    loop.latency_ticks = 3;
    loop.build(Vec2(600.f, 600.f));

    for (u32 i = 0; i < 40; ++i) loop.tick(Vec2(1.f, 0.f));

    loop.view.adopt_migrated_state(loop.player, Vec2(50.f, 50.f), Vec2());
    CHECK(distance_sq(loop.view.predicted_position(), loop.server_position()) > 10000.f);

    for (u32 i = 0; i < 80; ++i) loop.tick(Vec2(1.f, 0.f));

    f32 residual = (loop.view.predicted_position() - loop.server_position()).length();
    CHECK(residual < 60.f);
}

TEST_CASE(corrections_are_absorbed_smoothly_instead_of_snapping) {
    Loop loop;
    loop.latency_ticks = 4;
    loop.build(Vec2(500.f, 500.f), 0);

    for (u32 i = 0; i < 30; ++i) loop.tick(Vec2(1.f, 0.f));

    std::vector<Vec2> render_path;
    for (u32 c = 2; c <= 12; ++c) loop.world.spawn_player(c, Vec2(560.f, 500.f));

    for (u32 i = 0; i < 120; ++i) {
        loop.tick(Vec2(1.f, 0.f));
        render_path.push_back(loop.view.render_position());
    }

    f32 largest_step = 0.f;
    for (std::size_t i = 1; i < render_path.size(); ++i) {
        f32 step = (render_path[i] - render_path[i - 1]).length();
        if (step > largest_step) largest_step = step;
    }

    f32 max_travel_per_tick = loop.world.params().max_speed * loop.world.params().tick_dt();
    std::printf("       largest render step=%.3f, free travel per tick=%.3f\n", largest_step,
                max_travel_per_tick);
    CHECK(largest_step < max_travel_per_tick * 2.f);
}

TEST_CASE(prediction_survives_heavy_packet_loss) {
    Loop loop;
    loop.latency_ticks = 4;
    loop.loss = 0.3f;
    loop.build(Vec2(700.f, 700.f));

    std::mt19937 rng(5);
    std::uniform_real_distribution<f32> axis(-1.f, 1.f);
    for (u32 i = 0; i < 600; ++i) loop.tick(Vec2(axis(rng), axis(rng)));

    f32 residual = (loop.view.predicted_position() - loop.server_position()).length();
    std::printf("       residual under 30%% loss=%.3f, rejected=%u\n", residual,
                loop.view.stats().snapshots_rejected);

    CHECK(residual < 80.f);
    CHECK_EQ(loop.view.stats().snapshots_rejected, 0u);
}

TEST_CASE(remote_entities_are_interpolated_between_snapshots_not_teleported) {
    Loop loop;
    loop.latency_ticks = 2;
    loop.build(Vec2(1024.f, 1024.f));

    EntityId other = loop.world.spawn_player(2, Vec2(1100.f, 1024.f));

    for (u32 i = 0; i < 20; ++i) {
        MoveInput input;
        input.sequence = i + 1;
        input.tick = loop.now;
        input.move_y = 0.15f;
        loop.world.queue_input(2, input);
        loop.tick(Vec2());
    }

    std::vector<Vec2> path;
    for (u32 i = 0; i < 90; ++i) {
        MoveInput input;
        input.sequence = 100 + i;
        input.tick = loop.now;
        input.move_y = 0.15f;
        loop.world.queue_input(2, input);
        loop.tick(Vec2());

        for (const RenderEntity& entity : loop.view.render_entities()) {
            if (entity.id == other) path.push_back(entity.position);
        }
    }

    CHECK(path.size() > 60);

    f32 largest_step = 0.f;
    u32 distinct_positions = 0;
    for (std::size_t i = 1; i < path.size(); ++i) {
        f32 step = (path[i] - path[i - 1]).length();
        if (step > 0.0001f) ++distinct_positions;
        if (step > largest_step) largest_step = step;
    }

    f32 max_travel_per_tick = loop.world.params().max_speed * loop.world.params().tick_dt();
    std::printf("       remote largest step=%.3f, per-tick travel=%.3f, moved frames=%u\n",
                largest_step, max_travel_per_tick, distinct_positions);

    CHECK(largest_step < max_travel_per_tick * 1.5f);
    CHECK(distinct_positions > path.size() / 2);
}

TEST_CASE(interpolation_renders_behind_the_newest_snapshot) {
    Loop loop;
    loop.latency_ticks = 2;
    loop.build(Vec2(1024.f, 1024.f));

    EntityId other = loop.world.spawn_player(2, Vec2(1100.f, 1024.f));

    for (u32 i = 0; i < 100; ++i) {
        MoveInput input;
        input.sequence = i + 1;
        input.tick = loop.now;
        input.move_x = 0.12f;
        loop.world.queue_input(2, input);
        loop.tick(Vec2());
    }

    Vec2 rendered;
    bool found = false;
    for (const RenderEntity& entity : loop.view.render_entities()) {
        if (entity.id == other) {
            rendered = entity.position;
            found = true;
        }
    }
    CHECK(found);

    i64 index = loop.world.entities().find(other);
    Vec2 authoritative = loop.world.entities().position[static_cast<u32>(index)];

    CHECK(rendered.x < authoritative.x);
    CHECK(authoritative.x - rendered.x < 100.f);
}

TEST_CASE(input_history_is_trimmed_by_server_acknowledgements) {
    Loop loop;
    loop.latency_ticks = 3;
    loop.build(Vec2(400.f, 400.f));

    for (u32 i = 0; i < 200; ++i) loop.tick(Vec2(0.5f, 0.5f));

    CHECK(loop.view.stats().pending_inputs > 0u);
    CHECK(loop.view.stats().pending_inputs < 20u);
    CHECK(loop.view.last_acknowledged_input() > 150u);
}

TEST_MAIN()
