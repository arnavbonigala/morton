#include <cstring>
#include <random>
#include <vector>

#include "sim/world.h"
#include "tests/check.h"

using namespace morton;

namespace {

WorldParams test_params() {
    WorldParams params;
    params.size = 1024.f;
    params.tick_rate = 30;
    return params;
}

MoveInput make_input(u32 sequence, Tick tick, f32 x, f32 y) {
    MoveInput input;
    input.sequence = sequence;
    input.tick = tick;
    input.move_x = x;
    input.move_y = y;
    return input;
}

}  // namespace

TEST_CASE(identical_input_streams_produce_bit_identical_worlds) {
    World a;
    World b;
    a.configure(test_params(), 32);
    b.configure(test_params(), 32);

    std::vector<ClientId> clients;
    std::mt19937 rng(4);
    std::uniform_real_distribution<f32> coord(50.f, 900.f);

    for (u32 c = 1; c <= 40; ++c) {
        Vec2 start(coord(rng), coord(rng));
        a.spawn_player(c, start);
        b.spawn_player(c, start);
        clients.push_back(c);
    }
    for (int d = 0; d < 60; ++d) {
        Vec2 start(coord(rng), coord(rng));
        a.spawn_drifter(start);
        b.spawn_drifter(start);
    }

    std::mt19937 input_rng(9);
    std::uniform_real_distribution<f32> axis(-1.f, 1.f);

    for (u32 tick = 0; tick < 300; ++tick) {
        for (ClientId client : clients) {
            MoveInput input = make_input(tick + 1, tick, axis(input_rng), axis(input_rng));
            a.queue_input(client, input);
            b.queue_input(client, input);
        }
        a.step();
        b.step();
    }

    CHECK_EQ(a.entities().size(), b.entities().size());
    u32 mismatches = 0;
    for (u32 i = 0; i < a.entities().size(); ++i) {
        if (std::memcmp(&a.entities().position[i], &b.entities().position[i], sizeof(Vec2)) != 0 ||
            std::memcmp(&a.entities().velocity[i], &b.entities().velocity[i], sizeof(Vec2)) != 0) {
            ++mismatches;
        }
    }
    CHECK_EQ(mismatches, 0u);
}

TEST_CASE(client_side_prediction_matches_server_exactly) {
    World server;
    server.configure(test_params(), 32);
    const Vec2 start(400.f, 400.f);
    server.spawn_player(1, start);

    MoveState predicted{start, Vec2()};
    const WorldParams params = server.params();
    std::mt19937 rng(77);
    std::uniform_real_distribution<f32> axis(-1.f, 1.f);

    for (u32 tick = 0; tick < 200; ++tick) {
        MoveInput input = make_input(tick + 1, tick, axis(rng), axis(rng));
        server.queue_input(1, input);
        server.step();

        MoveInput quantized = input;
        quantize_input(&quantized);
        step_movement(&predicted, quantized, params, params.tick_dt());
    }

    i64 index = server.entities().find(1);
    CHECK(index >= 0);
    const Vec2& authoritative = server.entities().position[static_cast<u32>(index)];

    CHECK(std::memcmp(&authoritative, &predicted.position, sizeof(Vec2)) == 0);
}

TEST_CASE(unquantized_prediction_drifts_which_is_why_quantization_is_shared) {
    const WorldParams params = test_params();
    MoveState quantized_state{Vec2(400.f, 400.f), Vec2()};
    MoveState raw_state{Vec2(400.f, 400.f), Vec2()};

    for (u32 tick = 0; tick < 40; ++tick) {
        MoveInput raw = make_input(tick + 1, tick, 0.3777f, 0.6123f);
        MoveInput quantized = raw;
        quantize_input(&quantized);
        step_movement(&quantized_state, quantized, params, params.tick_dt());
        step_movement(&raw_state, raw, params, params.tick_dt());
    }

    CHECK(distance_sq(quantized_state.position, raw_state.position) > 0.f);
}

TEST_CASE(players_are_pushed_apart_and_never_left_overlapping) {
    World world;
    world.configure(test_params(), 32);

    for (u32 c = 1; c <= 30; ++c) world.spawn_player(c, Vec2(500.f, 500.f));

    for (int i = 0; i < 120; ++i) world.step();

    const EntityStore& entities = world.entities();
    f32 diameter = world.params().player_radius * 2.f;
    u32 overlapping = 0;
    for (u32 i = 0; i < entities.size(); ++i) {
        for (u32 j = i + 1; j < entities.size(); ++j) {
            f32 gap = distance_sq(entities.position[i], entities.position[j]);
            if (gap < diameter * diameter * 0.98f) ++overlapping;
        }
    }
    CHECK_EQ(overlapping, 0u);
}

TEST_CASE(entities_never_escape_world_bounds) {
    World world;
    world.configure(test_params(), 32);
    const WorldParams params = world.params();

    for (u32 c = 1; c <= 20; ++c) world.spawn_player(c, Vec2(20.f + c, 20.f));

    for (u32 tick = 0; tick < 400; ++tick) {
        for (u32 c = 1; c <= 20; ++c) {
            world.queue_input(c, make_input(tick + 1, tick, -1.f, -1.f));
        }
        world.step();
    }

    const EntityStore& entities = world.entities();
    for (u32 i = 0; i < entities.size(); ++i) {
        CHECK(entities.position[i].x >= params.player_radius - 0.001f);
        CHECK(entities.position[i].y >= params.player_radius - 0.001f);
        CHECK(entities.position[i].x <= params.size - params.player_radius + 0.001f);
        CHECK(entities.position[i].y <= params.size - params.player_radius + 0.001f);
    }
}

TEST_CASE(replayed_and_out_of_order_inputs_are_rejected) {
    World world;
    world.configure(test_params(), 32);
    world.spawn_player(1, Vec2(200.f, 200.f));

    world.queue_input(1, make_input(5, 0, 1.f, 0.f));
    world.queue_input(1, make_input(5, 0, 1.f, 0.f));
    world.queue_input(1, make_input(3, 0, -1.f, 0.f));
    world.queue_input(1, make_input(6, 0, 1.f, 0.f));

    const PlayerSlot* slot = world.player_of(1);
    CHECK(slot != nullptr);
    CHECK_EQ(slot->pending.size(), std::size_t{2});
    CHECK_EQ(slot->dropped_stale_inputs, 2u);
    CHECK_EQ(slot->highest_received_sequence, 6u);
}

TEST_CASE(starved_player_repeats_input_briefly_then_coasts_to_a_stop) {
    World world;
    world.configure(test_params(), 32);
    world.spawn_player(1, Vec2(500.f, 500.f));

    for (u32 tick = 0; tick < 30; ++tick) {
        world.queue_input(1, make_input(tick + 1, tick, 1.f, 0.f));
        world.step();
    }

    i64 index = world.entities().find(1);
    CHECK(index >= 0);
    f32 moving_speed = world.entities().velocity[static_cast<u32>(index)].length();
    CHECK(moving_speed > 50.f);

    for (int i = 0; i < 200; ++i) world.step();

    index = world.entities().find(1);
    f32 coasting_speed = world.entities().velocity[static_cast<u32>(index)].length();
    CHECK(coasting_speed < 1.f);
}

TEST_CASE(swap_and_pop_removal_keeps_the_index_map_consistent) {
    World world;
    world.configure(test_params(), 32);

    std::vector<EntityId> spawned;
    for (u32 c = 1; c <= 50; ++c) spawned.push_back(world.spawn_player(c, Vec2(100.f + c, 100.f)));

    std::mt19937 rng(13);
    std::shuffle(spawned.begin(), spawned.end(), rng);

    for (u32 i = 0; i < 25; ++i) {
        world.despawn(spawned[i]);
        world.step();
    }

    const EntityStore& entities = world.entities();
    CHECK_EQ(entities.size(), 25u);
    CHECK_EQ(entities.index_of.size(), std::size_t{25});
    for (u32 i = 0; i < entities.size(); ++i) {
        auto it = entities.index_of.find(entities.id[i]);
        CHECK(it != entities.index_of.end());
        CHECK_EQ(it->second, i);
    }
    for (u32 i = 0; i < 25; ++i) CHECK(world.entities().find(spawned[i]) < 0);
    for (u32 i = 25; i < 50; ++i) CHECK(world.entities().find(spawned[i]) >= 0);
}

TEST_CASE(adopted_entity_resumes_at_the_exact_migrated_state) {
    World world;
    world.configure(test_params(), 32);

    const Vec2 position(777.5f, 321.25f);
    const Vec2 velocity(-40.5f, 12.25f);
    world.adopt_entity(9001, 55, position, velocity, 1234);

    i64 index = world.entities().find(9001);
    CHECK(index >= 0);
    CHECK(world.entities().position[static_cast<u32>(index)] == position);
    CHECK(world.entities().velocity[static_cast<u32>(index)] == velocity);

    const PlayerSlot* slot = world.player_of(55);
    CHECK(slot != nullptr);
    CHECK_EQ(slot->last_applied_sequence, 1234u);
    CHECK_EQ(slot->entity, 9001u);

    world.queue_input(55, make_input(1200, 0, 1.f, 0.f));
    CHECK_EQ(world.player_of(55)->pending.size(), std::size_t{0});
    world.queue_input(55, make_input(1235, 0, 1.f, 0.f));
    CHECK_EQ(world.player_of(55)->pending.size(), std::size_t{1});
}

TEST_MAIN()
