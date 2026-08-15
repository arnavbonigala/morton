#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "check.h"
#include "cluster/migration.h"
#include "cluster/shard.h"
#include "redis_fixture.h"
#include "sim/region.h"
#include "sim/world.h"

using namespace morton;
using morton_test::redis_fixture;

namespace {

RegionMap test_regions() {
    RegionMap map;
    map.world_size = 2048.f;
    map.regions_per_axis = 2;
    map.margin = 48.f;
    return map;
}

ShardConfig shard_config(const std::string& id, u16 udp_port) {
    ShardConfig config;
    config.id = id;
    config.udp_endpoint = "127.0.0.1:" + std::to_string(udp_port);
    config.http_endpoint = "127.0.0.1:" + std::to_string(udp_port + 100);
    config.capacity = 500;
    config.heartbeat_ttl_ms = 60000;
    config.refresh_interval_ms = 0;
    config.presence_ttl_ms = 60000;
    config.ticket_ttl_ms = 60000;
    config.regions = test_regions();
    return config;
}

void wipe(const std::string& prefix) {
    RedisClient client;
    client.connect(redis_fixture().address());
    RedisReply keys = client.command({"KEYS", prefix + ":*"});
    if (keys.type != RedisType::kArray || keys.elements.empty()) return;
    std::vector<std::string> args{"DEL"};
    for (const RedisReply& key : keys.elements) args.push_back(key.str);
    client.command(args);
}

MigrationTicket sample_ticket() {
    MigrationTicket ticket;
    ticket.token = 0x0123456789abcdefull;
    ticket.player_id = "player-42";
    ticket.from_shard = "world-a";
    ticket.to_shard = "world-b";
    ticket.region = 3;
    ticket.tick = 918273;
    ticket.position = Vec2{1234.5f, 77.25f};
    ticket.velocity = Vec2{-64.125f, 12.5f};
    ticket.last_input_sequence = 40311;
    ticket.issued_ms = 1755200000000ull;
    return ticket;
}

}  // namespace

TEST_CASE(handoff_hysteresis_stops_a_player_ping_ponging_along_a_seam) {
    RegionMap map = test_regions();
    f32 seam = map.region_size();

    CHECK_EQ(map.region_of(10.f, 10.f), 0u);
    CHECK_EQ(map.region_of(seam + 10.f, 10.f), 1u);
    CHECK_EQ(map.region_of(10.f, seam + 10.f), 2u);
    CHECK_EQ(map.region_of(seam + 10.f, seam + 10.f), 3u);

    CHECK_EQ(map.handoff_target(0, seam - 1.f, 100.f), kInvalidRegion);
    CHECK_EQ(map.handoff_target(0, seam + map.margin - 1.f, 100.f), kInvalidRegion);
    CHECK_EQ(map.handoff_target(0, seam + map.margin + 1.f, 100.f), 1u);

    int handoffs = 0;
    u32 current = 0;
    for (int step = 0; step < 400; ++step) {
        f32 wobble = static_cast<f32>((step % 20) - 10) * 2.f;
        f32 x = seam + wobble;
        u32 target = map.handoff_target(current, x, 100.f);
        if (target != kInvalidRegion) {
            current = target;
            ++handoffs;
        }
    }
    CHECK_EQ(handoffs, 0);

    current = 0;
    handoffs = 0;
    for (int step = 0; step < 400; ++step) {
        f32 x = seam + static_cast<f32>((step % 200) - 100) * 2.f;
        u32 target = map.handoff_target(current, x, 100.f);
        if (target != kInvalidRegion) {
            current = target;
            ++handoffs;
        }
    }
    CHECK_EQ(handoffs, 3);
}

TEST_CASE(migration_tickets_reject_truncation_corruption_and_wrong_versions) {
    MigrationTicket original = sample_ticket();
    std::string blob = encode_migration_ticket(original);
    CHECK(!blob.empty());

    MigrationTicket decoded;
    CHECK(decode_migration_ticket(blob, &decoded));
    CHECK_EQ(decoded.token, original.token);
    CHECK(decoded.player_id == original.player_id);
    CHECK(decoded.from_shard == original.from_shard);
    CHECK(decoded.to_shard == original.to_shard);
    CHECK_EQ(decoded.region, original.region);
    CHECK_EQ(decoded.tick, original.tick);
    CHECK_EQ(decoded.last_input_sequence, original.last_input_sequence);
    CHECK_EQ(decoded.issued_ms, original.issued_ms);

    CHECK(std::memcmp(&decoded.position, &original.position, sizeof(Vec2)) == 0);
    CHECK(std::memcmp(&decoded.velocity, &original.velocity, sizeof(Vec2)) == 0);

    for (std::size_t cut = 1; cut < blob.size(); ++cut) {
        MigrationTicket out;
        CHECK(!decode_migration_ticket(blob.substr(0, cut), &out));
    }

    int accepted_corruptions = 0;
    for (std::size_t byte = 0; byte < blob.size(); ++byte) {
        for (int bit = 0; bit < 8; ++bit) {
            std::string flipped = blob;
            flipped[byte] = static_cast<char>(flipped[byte] ^ (1 << bit));
            MigrationTicket out;
            if (decode_migration_ticket(flipped, &out)) ++accepted_corruptions;
        }
    }
    CHECK_EQ(accepted_corruptions, 0);

    MigrationTicket huge = original;
    huge.player_id = std::string(600, 'x');
    CHECK(encode_migration_ticket(huge).empty());
}

TEST_CASE(ownership_is_agreed_without_election_and_survives_a_shard_dying) {
    wipe("morton:m1");

    ShardCoordinator a;
    ShardCoordinator b;
    ShardCoordinator c;
    CHECK(a.start(shard_config("world-a", 9001), redis_fixture().address(), "morton:m1"));
    CHECK(b.start(shard_config("world-b", 9002), redis_fixture().address(), "morton:m1"));
    CHECK(c.start(shard_config("world-c", 9003), redis_fixture().address(), "morton:m1"));

    a.refresh(1, 0, 0.0, true);
    b.refresh(1, 0, 0.0, true);
    c.refresh(1, 0, 0.0, true);

    RegionMap map = test_regions();
    std::set<std::string> owners;
    for (u32 region = 0; region < map.region_count(); ++region) {
        const std::string& owner = a.owner_of_region(region);
        CHECK(!owner.empty());
        CHECK(owner == b.owner_of_region(region));
        CHECK(owner == c.owner_of_region(region));
        owners.insert(owner);
    }
    CHECK_EQ(owners.size(), 3u);

    u32 total_owned = static_cast<u32>(a.owned_regions().size() + b.owned_regions().size() +
                                       c.owned_regions().size());
    CHECK_EQ(total_owned, map.region_count());

    std::string endpoint;
    CHECK(a.endpoint_of_shard("world-b", &endpoint));
    CHECK(endpoint == "127.0.0.1:9002");

    c.stop();

    a.refresh(2, 0, 0.0, true);
    b.refresh(2, 0, 0.0, true);

    owners.clear();
    for (u32 region = 0; region < map.region_count(); ++region) {
        const std::string& owner = a.owner_of_region(region);
        CHECK(owner == b.owner_of_region(region));
        CHECK(owner != "world-c");
        owners.insert(owner);
    }
    CHECK_EQ(owners.size(), 2u);
    CHECK_EQ(a.owned_regions().size() + b.owned_regions().size(), map.region_count());

    a.stop();
    b.stop();
}

TEST_CASE(a_handoff_ticket_can_only_be_redeemed_once) {
    wipe("morton:m2");

    ShardCoordinator source;
    ShardCoordinator destination;
    CHECK(source.start(shard_config("world-a", 9101), redis_fixture().address(), "morton:m2"));
    CHECK(destination.start(shard_config("world-b", 9102), redis_fixture().address(), "morton:m2"));
    source.refresh(1, 0, 0.0, true);
    destination.refresh(1, 0, 0.0, true);

    u32 foreign_region = kInvalidRegion;
    for (u32 region = 0; region < test_regions().region_count(); ++region) {
        if (source.owner_of_region(region) == "world-b") {
            foreign_region = region;
            break;
        }
    }
    CHECK(foreign_region != kInvalidRegion);

    PresenceRecord record;
    record.player_id = "player-mover";
    record.shard_id = "world-a";
    record.region = 0;
    record.session_token = 12345;
    std::string owner;
    CHECK(source.claim_player(record, &owner));

    RegionMap map = test_regions();
    f32 cx = 0, cy = 0;
    map.center_of(foreign_region, &cx, &cy);

    HandoffPlan plan;
    u32 source_region = kInvalidRegion;
    for (u32 region : source.owned_regions()) {
        source_region = region;
        break;
    }
    CHECK(source_region != kInvalidRegion);

    CHECK(source.plan_handoff("player-mover", source_region, Vec2{cx, cy}, Vec2{5.f, -3.f}, 4242,
                              777, &plan));
    CHECK(plan.target_shard == "world-b");
    CHECK(plan.target_endpoint == "127.0.0.1:9102");
    CHECK_EQ(plan.target_region, foreign_region);
    CHECK(plan.token != 0);

    PresenceRecord moved;
    CHECK(source.registry().get_presence("player-mover", &moved));
    CHECK(moved.shard_id == "world-b");

    MigrationTicket ticket;
    CHECK(destination.accept_handoff(plan.token, &ticket));
    CHECK(ticket.player_id == "player-mover");
    CHECK_EQ(ticket.last_input_sequence, 777u);
    CHECK_EQ(ticket.tick, 4242u);
    CHECK_NEAR(ticket.position.x, cx, 0.0001);
    CHECK_NEAR(ticket.velocity.y, -3.f, 0.0001);

    MigrationTicket replay;
    CHECK(!destination.accept_handoff(plan.token, &replay));
    CHECK(!source.accept_handoff(plan.token, &replay));
    CHECK_EQ(destination.handoffs_accepted(), 1u);
    CHECK(destination.tickets().rejected() >= 1u);

    CHECK(!destination.accept_handoff(0xdeadbeefull, &replay));

    source.release_player("player-mover");
    source.stop();
    destination.stop();
}

TEST_CASE(a_migrated_player_resumes_at_the_exact_state_it_left_with) {
    wipe("morton:m3");

    ShardCoordinator source;
    ShardCoordinator destination;
    CHECK(source.start(shard_config("world-a", 9201), redis_fixture().address(), "morton:m3"));
    CHECK(destination.start(shard_config("world-b", 9202), redis_fixture().address(), "morton:m3"));
    source.refresh(1, 0, 0.0, true);
    destination.refresh(1, 0, 0.0, true);

    WorldParams params;
    params.size = 2048.f;
    World world_a;
    World world_b;
    world_a.configure(params, 64);
    world_b.configure(params, 64);

    RegionMap map = test_regions();
    u32 home = source.owned_regions().front();
    f32 hx = 0, hy = 0;
    map.center_of(home, &hx, &hy);

    EntityId entity = world_a.spawn_player(7, Vec2{hx, hy});
    for (u32 sequence = 1; sequence <= 60; ++sequence) {
        MoveInput input;
        input.sequence = sequence;
        input.move_x = 1.f;
        input.move_y = 0.6f;
        quantize_input(&input);
        world_a.queue_input(7, input);
        world_a.step();
    }

    i64 index = world_a.entities().find(entity);
    CHECK(index >= 0);
    Vec2 position = world_a.entities().position[static_cast<std::size_t>(index)];
    Vec2 velocity = world_a.entities().velocity[static_cast<std::size_t>(index)];
    u32 last_sequence =
        world_a.entities().last_input_sequence[static_cast<std::size_t>(index)];
    CHECK(last_sequence > 0);

    PresenceRecord record;
    record.player_id = "player-9";
    record.shard_id = "world-a";
    record.region = home;
    record.session_token = 55;
    std::string owner;
    CHECK(source.claim_player(record, &owner));

    u32 foreign = kInvalidRegion;
    for (u32 region = 0; region < map.region_count(); ++region) {
        if (source.owner_of_region(region) == "world-b") foreign = region;
    }
    CHECK(foreign != kInvalidRegion);
    f32 fx = 0, fy = 0;
    map.center_of(foreign, &fx, &fy);

    HandoffPlan plan;
    CHECK(source.plan_handoff("player-9", home, Vec2{fx, fy}, velocity, world_a.tick(),
                              last_sequence, &plan));

    world_a.despawn(entity);
    CHECK(world_a.entities().find(entity) < 0);

    MigrationTicket ticket;
    CHECK(destination.accept_handoff(plan.token, &ticket));

    world_b.set_tick(ticket.tick);
    EntityId adopted = 4001;
    world_b.adopt_entity(adopted, 7, ticket.position, ticket.velocity,
                         ticket.last_input_sequence);

    i64 landed = world_b.entities().find(adopted);
    CHECK(landed >= 0);
    Vec2 resumed_velocity = world_b.entities().velocity[static_cast<std::size_t>(landed)];
    CHECK(std::memcmp(&resumed_velocity, &velocity, sizeof(Vec2)) == 0);
    CHECK_EQ(world_b.entities().last_input_sequence[static_cast<std::size_t>(landed)],
             last_sequence);
    CHECK_EQ(world_b.tick(), world_a.tick());

    const PlayerSlot* slot = world_b.player_of(7);
    CHECK(slot != nullptr);
    CHECK_EQ(slot->last_applied_sequence, last_sequence);

    MoveInput stale;
    stale.sequence = last_sequence;
    stale.move_x = 1.f;
    quantize_input(&stale);
    world_b.queue_input(7, stale);
    world_b.step();
    CHECK(world_b.stats().inputs_applied == 0);

    MoveInput fresh;
    fresh.sequence = last_sequence + 1;
    fresh.move_x = 1.f;
    quantize_input(&fresh);
    world_b.queue_input(7, fresh);
    world_b.step();
    CHECK(world_b.stats().inputs_applied == 1);

    PresenceRecord after;
    CHECK(destination.registry().get_presence("player-9", &after));
    CHECK(after.shard_id == "world-b");
    CHECK_EQ(after.session_token, 55u);

    (void)position;
    destination.release_player("player-9");
    source.stop();
    destination.stop();
}

int main() {
    if (!redis_fixture().start()) {
        std::printf("redis-server unavailable; skipping migration integration tests\n");
        return 0;
    }
    int result = ::morton_test::run_all();
    redis_fixture().stop();
    return result;
}
