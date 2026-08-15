#include <string>
#include <vector>

#include "check.h"
#include "cluster/presence.h"
#include "cluster/redis.h"
#include "redis_fixture.h"

using namespace morton;
using morton_test::redis_fixture;

namespace {

RedisClient& shared_client() {
    static RedisClient client;
    if (!client.connected()) client.connect(redis_fixture().address());
    return client;
}

void make_registry(ClusterRegistry* registry, const std::string& prefix) {
    registry->connect(redis_fixture().address(), prefix);
}

}  // namespace

TEST_CASE(resp_round_trips_every_reply_type_including_binary_payloads) {
    RedisClient& redis = shared_client();
    CHECK(redis.connected());

    std::string binary("head\r\n$5\r\nfake\r\n", 16);
    binary.push_back('\0');
    binary += "tail";

    CHECK(redis.command({"SET", "morton:test:binary", binary}).ok());
    RedisReply fetched = redis.command({"GET", "morton:test:binary"});
    CHECK(fetched.type == RedisType::kString);
    CHECK(fetched.str == binary);
    CHECK_EQ(fetched.str.size(), binary.size());

    CHECK(redis.command({"GET", "morton:test:absent"}).is_nil());

    RedisReply counted = redis.command({"INCRBY", "morton:test:counter", "41"});
    CHECK(counted.type == RedisType::kInteger);
    CHECK_EQ(counted.integer, 41);

    CHECK(redis.command({"INCRBY", "morton:test:binary", "1"}).is_error());

    redis.command({"DEL", "morton:test:list"});
    redis.command({"RPUSH", "morton:test:list", "a", "", "c"});
    RedisReply list = redis.command({"LRANGE", "morton:test:list", "0", "-1"});
    CHECK(list.type == RedisType::kArray);
    CHECK_EQ(list.elements.size(), 3u);
    CHECK(list.elements[0].str == "a");
    CHECK(list.elements[1].type == RedisType::kString);
    CHECK(list.elements[1].str.empty());
    CHECK(list.elements[2].str == "c");

    RedisReply nested = redis.command({"EVAL", "return {1, 'two', {3, 'four'}, 'five'}", "0"});
    CHECK(nested.type == RedisType::kArray);
    CHECK_EQ(nested.elements.size(), 4u);
    CHECK_EQ(nested.elements[0].integer, 1);
    CHECK(nested.elements[2].type == RedisType::kArray);
    CHECK(nested.elements[2].elements[1].str == "four");
    CHECK(nested.elements[3].str == "five");

    redis.command({"DEL", "morton:test:binary", "morton:test:counter", "morton:test:list"});
}

TEST_CASE(pipelining_preserves_command_order_across_a_single_write) {
    RedisClient redis;
    CHECK(redis.connect(redis_fixture().address()));

    redis.command({"DEL", "morton:pipe"});
    constexpr int kCommands = 500;
    for (int i = 0; i < kCommands; ++i) {
        redis.queue({"RPUSH", "morton:pipe", std::to_string(i)});
    }
    CHECK_EQ(redis.queued(), static_cast<std::size_t>(kCommands));

    std::vector<RedisReply> replies;
    CHECK(redis.flush(&replies));
    CHECK_EQ(replies.size(), static_cast<std::size_t>(kCommands));
    CHECK_EQ(redis.queued(), 0u);

    for (int i = 0; i < kCommands; ++i) {
        CHECK_EQ(replies[static_cast<std::size_t>(i)].integer, i + 1);
    }

    RedisReply stored = redis.command({"LRANGE", "morton:pipe", "0", "-1"});
    CHECK_EQ(stored.elements.size(), static_cast<std::size_t>(kCommands));
    CHECK(stored.elements.front().str == "0");
    CHECK(stored.elements.back().str == std::to_string(kCommands - 1));

    redis.command({"DEL", "morton:pipe"});
}

TEST_CASE(client_reconnects_transparently_after_the_link_drops) {
    RedisClient redis;
    CHECK(redis.connect(redis_fixture().address()));
    CHECK(redis.command({"SET", "morton:reconnect", "before"}).ok());
    CHECK_EQ(redis.reconnects(), 0u);

    redis.command({"QUIT"});

    RedisReply after = redis.command({"GET", "morton:reconnect"});
    CHECK(after.type == RedisType::kString);
    CHECK(after.str == "before");
    CHECK(redis.reconnects() >= 1u);
    CHECK(redis.connected());

    redis.command({"DEL", "morton:reconnect"});
}

TEST_CASE(expired_shards_disappear_from_discovery_and_are_pruned) {
    ClusterRegistry registry;
    make_registry(&registry, "morton:t1");
    CHECK(registry.connected());

    ShardInfo alive;
    alive.id = "world-a";
    alive.udp_endpoint = "127.0.0.1:9001";
    alive.http_endpoint = "127.0.0.1:9101";
    alive.player_count = 120;
    alive.capacity = 500;
    alive.tick_p99_ms = 4.5;

    ShardInfo doomed = alive;
    doomed.id = "world-b";
    doomed.udp_endpoint = "127.0.0.1:9002";
    doomed.player_count = 400;

    CHECK(registry.heartbeat_shard(alive, 60000));
    CHECK(registry.heartbeat_shard(doomed, 250));

    std::vector<ShardInfo> shards = registry.live_shards();
    CHECK_EQ(shards.size(), 2u);

    ShardInfo fetched;
    CHECK(registry.get_shard("world-a", &fetched));
    CHECK_EQ(fetched.player_count, 120u);
    CHECK_EQ(fetched.capacity, 500u);
    CHECK_NEAR(fetched.tick_p99_ms, 4.5, 0.001);
    CHECK(fetched.heartbeat_ms > 0);
    CHECK(fetched.accepting());
    CHECK_NEAR(fetched.load_factor(), 0.24, 0.001);

    sleep_us(500000);

    u32 pruned = 0;
    shards = registry.live_shards(&pruned);
    CHECK_EQ(shards.size(), 1u);
    CHECK(shards[0].id == "world-a");
    CHECK_EQ(pruned, 1u);

    RedisReply members = registry.client().command({"SMEMBERS", "morton:t1:shards"});
    CHECK_EQ(members.elements.size(), 1u);

    registry.remove_shard("world-a");
}

TEST_CASE(presence_claims_are_exclusive_and_transfers_are_compare_and_set) {
    ClusterRegistry registry;
    make_registry(&registry, "morton:t2");
    CHECK(registry.connected());
    registry.clear_presence("player-7");

    PresenceRecord record;
    record.player_id = "player-7";
    record.shard_id = "world-a";
    record.region = 2;
    record.session_token = 0xfeedfaceull;

    std::string owner;
    CHECK(registry.claim_presence(record, 60000, &owner));

    PresenceRecord stolen = record;
    stolen.shard_id = "world-b";
    owner.clear();
    CHECK(!registry.claim_presence(stolen, 60000, &owner));
    CHECK(owner == "world-a");

    PresenceRecord read;
    CHECK(registry.get_presence("player-7", &read));
    CHECK(read.shard_id == "world-a");
    CHECK_EQ(read.region, 2u);
    CHECK_EQ(read.session_token, 0xfeedfaceull);

    std::vector<std::string> roster = registry.players_on_shard("world-a");
    CHECK_EQ(roster.size(), 1u);
    CHECK(roster[0] == "player-7");

    CHECK(!registry.transfer_presence("player-7", "world-c", "world-b", 3, 60000));
    CHECK(registry.get_presence("player-7", &read));
    CHECK(read.shard_id == "world-a");

    CHECK(registry.transfer_presence("player-7", "world-a", "world-b", 3, 60000));
    CHECK(registry.get_presence("player-7", &read));
    CHECK(read.shard_id == "world-b");
    CHECK_EQ(read.region, 3u);
    CHECK_EQ(read.session_token, 0xfeedfaceull);

    CHECK(registry.players_on_shard("world-a").empty());
    CHECK_EQ(registry.players_on_shard("world-b").size(), 1u);

    CHECK(!registry.transfer_presence("player-7", "world-a", "world-c", 4, 60000));
    CHECK(registry.get_presence("player-7", &read));
    CHECK(read.shard_id == "world-b");

    registry.clear_presence("player-7");
    CHECK(!registry.get_presence("player-7", &read));
    CHECK(registry.players_on_shard("world-b").empty());
}

TEST_CASE(presence_expires_when_a_shard_stops_refreshing_it) {
    ClusterRegistry registry;
    make_registry(&registry, "morton:t3");
    CHECK(registry.connected());

    PresenceRecord record;
    record.player_id = "player-transient";
    record.shard_id = "world-a";
    record.region = 1;
    record.session_token = 99;

    CHECK(registry.set_presence(record, 250));
    PresenceRecord read;
    CHECK(registry.get_presence("player-transient", &read));

    sleep_us(500000);
    CHECK(!registry.get_presence("player-transient", &read));

    CHECK(registry.set_presence(record, 60000));
    CHECK(registry.get_presence("player-transient", &read));
    CHECK(read.shard_id == "world-a");
    registry.clear_presence("player-transient");
}

int main() {
    if (!redis_fixture().start()) {
        std::printf("redis-server unavailable; skipping redis integration tests\n");
        return 0;
    }
    int result = ::morton_test::run_all();
    redis_fixture().stop();
    return result;
}
