#include <algorithm>
#include <random>
#include <set>
#include <vector>

#include "proto/snapshot.h"
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

/// Minimal receiver: keeps decoded frames so later deltas have a baseline, and
/// mirrors what a real client does when it acks.
struct Receiver {
    SnapshotHistory history;
    ReplicationQuantizers quantizers;
    DecodedSnapshot latest;
    bool has_latest = false;

    void configure(const WorldParams& params, u32 capacity) {
        history.configure(capacity);
        quantizers = ReplicationQuantizers::from(params);
    }

    bool apply(const u8* data, u32 size) {
        Tick tick = 0;
        Tick baseline_tick = 0;
        bool full = false;
        if (!peek_snapshot_header(data, size, &tick, &baseline_tick, &full)) return false;

        const SnapshotFrame* baseline = full ? nullptr : history.get(baseline_tick);
        if (!full && baseline == nullptr) return false;

        DecodedSnapshot decoded;
        if (!decode_snapshot(data, size, quantizers, baseline, &decoded)) return false;

        SnapshotFrame* frame = history.slot(decoded.tick);
        frame->tick = decoded.tick;
        frame->valid = true;
        frame->states = decoded.states;

        latest = std::move(decoded);
        has_latest = true;
        return true;
    }
};

struct Harness {
    World world;
    ReplicationState replication;
    Receiver receiver;
    EntityId viewer = kInvalidEntity;
    InterestConfig config;

    void build(u32 drifters, f32 aoi_radius = 260.f, u32 max_bytes = 1000,
               u32 baseline_history = 32) {
        WorldParams params = test_params();
        world.configure(params, 32);

        config.aoi_radius = aoi_radius;
        config.max_snapshot_bytes = max_bytes;
        config.baseline_history = baseline_history;
        replication.configure(config, params);
        receiver.configure(params, config.baseline_history);

        viewer = world.spawn_player(1, Vec2(1024.f, 1024.f));

        std::mt19937 rng(2024);
        std::uniform_real_distribution<f32> coord(20.f, params.size - 20.f);
        for (u32 i = 0; i < drifters; ++i) world.spawn_drifter(Vec2(coord(rng), coord(rng)));

        world.step();
    }

    /// Encodes one snapshot and hands it to the receiver. Returns bytes on the wire.
    u32 exchange(bool deliver = true, bool ack = true) {
        u8 packet[kMaxDatagramSizeForTest];
        u32 size = replication.encode(world, viewer, 0, packet, sizeof(packet));
        if (size == 0) return 0;
        if (deliver && receiver.apply(packet, size) && ack) {
            replication.acknowledge(receiver.latest.tick);
        }
        return size;
    }

    static constexpr u32 kMaxDatagramSizeForTest = 1200;

    /// Entities the server considers relevant to the viewer this tick.
    std::set<EntityId> relevant_set() const {
        std::set<EntityId> result;
        const EntityStore& entities = world.entities();
        i64 index = entities.find(viewer);
        Vec2 eye = entities.position[static_cast<u32>(index)];
        for (u32 i = 0; i < entities.size(); ++i) {
            if (distance_sq(entities.position[i], eye) <= config.aoi_radius * config.aoi_radius) {
                result.insert(entities.id[i]);
            }
        }
        return result;
    }
};

std::set<EntityId> ids_of(const std::vector<EntityWireState>& states) {
    std::set<EntityId> result;
    for (const EntityWireState& state : states) result.insert(state.id);
    return result;
}

}  // namespace

TEST_CASE(full_snapshot_then_deltas_keep_the_client_in_sync) {
    Harness harness;
    harness.build(400);

    CHECK(harness.exchange() > 0);
    CHECK(harness.receiver.latest.full);

    for (int tick = 0; tick < 200; ++tick) {
        harness.world.step();
        CHECK(harness.exchange() > 0);
    }

    CHECK(!harness.receiver.latest.full);
    CHECK(harness.receiver.latest.tick == harness.world.tick());

    const ReplicationQuantizers quantizers = ReplicationQuantizers::from(harness.world.params());
    const EntityStore& entities = harness.world.entities();
    u32 compared = 0;

    for (const EntityWireState& state : harness.receiver.latest.states) {
        i64 index = entities.find(state.id);
        CHECK(index >= 0);
        if (index < 0) continue;
        CHECK_EQ(state.px, quantizers.position.encode(entities.position[static_cast<u32>(index)].x));
        CHECK_EQ(state.py, quantizers.position.encode(entities.position[static_cast<u32>(index)].y));
        ++compared;
    }
    CHECK(compared > 5);
    CHECK(ids_of(harness.receiver.latest.states) == harness.relevant_set());
}

TEST_CASE(entities_outside_the_area_of_interest_are_never_replicated) {
    Harness harness;
    harness.build(600, 200.f);

    for (int tick = 0; tick < 120; ++tick) {
        harness.world.step();
        harness.exchange();

        std::set<EntityId> relevant = harness.relevant_set();
        for (const EntityWireState& state : harness.receiver.latest.states) {
            CHECK(relevant.count(state.id) == 1);
        }
    }

    CHECK(harness.receiver.latest.states.size() < harness.world.entities().size() / 4);
}

TEST_CASE(entities_leaving_the_area_of_interest_are_removed_from_the_client) {
    WorldParams params = test_params();
    World world;
    world.configure(params, 32);

    InterestConfig config;
    config.aoi_radius = 150.f;

    ReplicationState replication;
    replication.configure(config, params);
    Receiver receiver;
    receiver.configure(params, config.baseline_history);

    EntityId viewer = world.spawn_player(1, Vec2(500.f, 500.f));
    EntityId walker = world.spawn_player(2, Vec2(560.f, 500.f));
    world.step();

    u8 packet[1200];
    u32 size = replication.encode(world, viewer, 0, packet, sizeof(packet));
    CHECK(receiver.apply(packet, size));
    replication.acknowledge(receiver.latest.tick);
    CHECK(ids_of(receiver.latest.states).count(walker) == 1);

    for (u32 tick = 0; tick < 120; ++tick) {
        MoveInput input;
        input.sequence = tick + 1;
        input.tick = tick;
        input.move_x = 1.f;
        world.queue_input(2, input);
        world.step();

        size = replication.encode(world, viewer, 0, packet, sizeof(packet));
        CHECK(size > 0);
        CHECK(receiver.apply(packet, size));
        replication.acknowledge(receiver.latest.tick);
    }

    i64 walker_index = world.entities().find(walker);
    Vec2 walker_position = world.entities().position[static_cast<u32>(walker_index)];
    CHECK(distance_sq(walker_position, Vec2(500.f, 500.f)) > config.aoi_radius * config.aoi_radius);
    CHECK(ids_of(receiver.latest.states).count(walker) == 0);
}

TEST_CASE(dropped_snapshots_do_not_corrupt_the_client_view) {
    Harness harness;
    harness.build(400);

    std::mt19937 rng(88);
    u32 decoded_count = 0;
    u32 rejected_count = 0;

    for (int tick = 0; tick < 400; ++tick) {
        harness.world.step();

        u8 packet[1200];
        u32 size = harness.replication.encode(harness.world, harness.viewer, 0, packet,
                                              sizeof(packet));
        CHECK(size > 0);

        bool delivered = std::uniform_real_distribution<f32>(0.f, 1.f)(rng) > 0.3f;
        if (!delivered) continue;

        if (harness.receiver.apply(packet, size)) {
            ++decoded_count;
            harness.replication.acknowledge(harness.receiver.latest.tick);
        } else {
            ++rejected_count;
        }
    }

    CHECK(decoded_count > 200);
    CHECK_EQ(rejected_count, 0u);
    CHECK(ids_of(harness.receiver.latest.states) == harness.relevant_set());
}

TEST_CASE(client_recovers_when_its_baseline_ages_out_of_history) {
    Harness harness;
    harness.build(300);

    CHECK(harness.exchange() > 0);

    for (int tick = 0; tick < 100; ++tick) {
        harness.world.step();
        harness.exchange(false, false);
    }

    for (int tick = 0; tick < 60; ++tick) {
        harness.world.step();
        harness.exchange();
    }

    CHECK(harness.receiver.has_latest);
    CHECK(ids_of(harness.receiver.latest.states) == harness.relevant_set());
}

TEST_CASE(a_constrained_packet_defers_entities_without_starving_them) {
    Harness harness;
    harness.build(0, 400.f, 90);

    std::vector<EntityId> crowd;
    for (u32 c = 2; c <= 60; ++c) {
        f32 angle = static_cast<f32>(c) * 0.31f;
        crowd.push_back(harness.world.spawn_player(
            c, Vec2(1024.f + std::cos(angle) * 120.f, 1024.f + std::sin(angle) * 120.f)));
    }
    harness.world.step();

    std::set<EntityId> ever_seen;
    bool saw_deferral = false;

    for (int tick = 0; tick < 200; ++tick) {
        harness.world.step();
        CHECK(harness.exchange() > 0);
        if (harness.replication.stats().entities_deferred > 0) saw_deferral = true;
        for (const EntityWireState& state : harness.receiver.latest.states) {
            ever_seen.insert(state.id);
        }
    }

    CHECK(saw_deferral);
    for (EntityId id : crowd) CHECK(ever_seen.count(id) == 1);
    CHECK(ever_seen.count(harness.viewer) == 1);
}

TEST_CASE(the_viewers_own_entity_is_always_replicated_for_reconciliation) {
    Harness harness;
    harness.build(0, 400.f, 80);

    for (u32 c = 2; c <= 80; ++c) {
        f32 angle = static_cast<f32>(c) * 0.19f;
        harness.world.spawn_player(
            c, Vec2(1024.f + std::cos(angle) * 60.f, 1024.f + std::sin(angle) * 60.f));
    }
    harness.world.step();

    for (int tick = 0; tick < 100; ++tick) {
        harness.world.step();
        CHECK(harness.exchange() > 0);
        CHECK(ids_of(harness.receiver.latest.states).count(harness.viewer) == 1);
    }
}

TEST_CASE(relevance_and_delta_encoding_cut_bandwidth_against_full_state_broadcast) {
    Harness harness;
    harness.build(2000);

    u64 delta_bytes = 0;
    u64 naive_bytes = 0;
    const u32 kTicks = 300;

    for (u32 tick = 0; tick < kTicks; ++tick) {
        harness.world.step();
        delta_bytes += harness.exchange();

        // Uncompressed broadcast of every entity: id, position and velocity as floats.
        naive_bytes += static_cast<u64>(harness.world.entities().size()) * (4 + 8 + 8);
    }

    f64 reduction = 100.0 * (1.0 - static_cast<f64>(delta_bytes) / static_cast<f64>(naive_bytes));
    std::printf("       delta=%llu bytes  naive=%llu bytes  reduction=%.2f%%\n",
                static_cast<unsigned long long>(delta_bytes),
                static_cast<unsigned long long>(naive_bytes), reduction);
    std::printf("       per-client bitrate: %.1f kbit/s at %u Hz\n",
                static_cast<f64>(delta_bytes) / (kTicks / 30.0) * 8.0 / 1000.0, 30u);

    CHECK(reduction > 90.0);
}

TEST_CASE(snapshots_never_exceed_the_configured_budget) {
    Harness harness;
    harness.build(3000, 500.f, 900);

    for (int tick = 0; tick < 150; ++tick) {
        harness.world.step();
        u32 size = harness.exchange();
        CHECK(size > 0);
        CHECK(size <= 900u);
    }
}

TEST_CASE(malformed_snapshots_are_rejected_without_crashing) {
    Harness harness;
    harness.build(200);
    harness.exchange();

    std::mt19937 rng(4242);
    ReplicationQuantizers quantizers = ReplicationQuantizers::from(harness.world.params());

    for (int i = 0; i < 5000; ++i) {
        u8 junk[256];
        u32 size = 1 + (rng() % sizeof(junk));
        for (u32 b = 0; b < size; ++b) junk[b] = static_cast<u8>(rng());

        DecodedSnapshot decoded;
        decode_snapshot(junk, size, quantizers, nullptr, &decoded);
    }
}

TEST_MAIN()

TEST_CASE(a_baseline_that_ages_out_of_the_ring_is_abandoned_not_reused) {
    Harness harness;
    harness.build(400, 260.f, 1000, 8);

    CHECK(harness.exchange() > 0);
    CHECK(harness.receiver.latest.full);

    // The client stops acking, so the server's baseline stays put until the ring
    // wraps onto the slot the next frame is about to occupy.
    for (u32 i = 0; i < 8; ++i) {
        harness.world.step();
        u8 packet[1200];
        u32 size = harness.replication.encode(harness.world, harness.viewer, 0, packet,
                                              sizeof(packet));
        CHECK(size > 0);
        if (i + 1 < 8) continue;

        Tick tick = 0;
        Tick baseline_tick = 0;
        bool full = false;
        CHECK(peek_snapshot_header(packet, size, &tick, &baseline_tick, &full));
        CHECK(full);
        CHECK(harness.receiver.apply(packet, size));
        CHECK(ids_of(harness.receiver.latest.states) == harness.relevant_set());
    }
}
