#include <map>
#include <thread>
#include <string>
#include <vector>

#include "check.h"
#include "cluster/matchmaker.h"
#include "http_client.h"
#include "core/time.h"
#include "redis_fixture.h"

using namespace morton;
using morton_test::http_body;
using morton_test::http_request;
using morton_test::json_field;
using morton_test::redis_fixture;

namespace {

void wipe(const std::string& prefix) {
    RedisClient client;
    client.connect(redis_fixture().address());
    RedisReply keys = client.command({"KEYS", prefix + ":*"});
    if (keys.type != RedisType::kArray || keys.elements.empty()) return;
    std::vector<std::string> args{"DEL"};
    for (const RedisReply& key : keys.elements) args.push_back(key.str);
    client.command(args);
}

ShardInfo shard(const std::string& id, u16 port, u32 players, u32 capacity,
                bool draining = false) {
    ShardInfo info;
    info.id = id;
    info.udp_endpoint = "127.0.0.1:" + std::to_string(port);
    info.http_endpoint = "127.0.0.1:" + std::to_string(port + 100);
    info.player_count = players;
    info.capacity = capacity;
    info.tick_p99_ms = 3.25;
    info.draining = draining;
    return info;
}

MatchmakerConfig config_for(const std::string& prefix) {
    MatchmakerConfig config;
    config.redis = redis_fixture().address();
    config.http_bind = Address(0x7f000001u, 0);
    config.key_prefix = prefix;
    config.session_ttl_ms = 60000;
    return config;
}

ClusterRegistry& admin(const std::string& prefix) {
    static std::map<std::string, ClusterRegistry*> registries;
    auto it = registries.find(prefix);
    if (it == registries.end()) {
        ClusterRegistry* registry = new ClusterRegistry();
        registry->connect(redis_fixture().address(), prefix);
        it = registries.emplace(prefix, registry).first;
    }
    return *it->second;
}

}  // namespace

TEST_CASE(assignment_follows_shard_load_and_refuses_when_nothing_can_accept) {
    wipe("morton:mm1");
    ClusterRegistry& registry = admin("morton:mm1");

    Matchmaker matchmaker;
    CHECK(matchmaker.start(config_for("morton:mm1")));

    SessionGrant nothing = matchmaker.assign("player-early");
    CHECK(!nothing.ok);
    CHECK(nothing.reason == "no live shards");

    registry.heartbeat_shard(shard("world-a", 9001, 450, 500), 60000);
    registry.heartbeat_shard(shard("world-b", 9002, 10, 500), 60000);
    registry.heartbeat_shard(shard("world-c", 9003, 200, 500), 60000);

    SessionGrant grant = matchmaker.assign("player-1");
    CHECK(grant.ok);
    CHECK(!grant.reconnect);
    CHECK(grant.shard_id == "world-b");
    CHECK(grant.udp_endpoint == "127.0.0.1:9002");
    CHECK(grant.session_token != 0);

    registry.heartbeat_shard(shard("world-b", 9002, 480, 500), 60000);
    SessionGrant next = matchmaker.assign("player-2");
    CHECK(next.ok);
    CHECK(next.shard_id == "world-c");

    registry.heartbeat_shard(shard("world-a", 9001, 500, 500), 60000);
    registry.heartbeat_shard(shard("world-b", 9002, 500, 500), 60000);
    registry.heartbeat_shard(shard("world-c", 9003, 10, 500, true), 60000);

    SessionGrant refused = matchmaker.assign("player-3");
    CHECK(!refused.ok);
    CHECK(refused.reason == "all shards full or draining");
    CHECK(matchmaker.rejections() >= 2u);

    matchmaker.stop();
}

TEST_CASE(a_returning_player_is_steered_back_to_the_shard_that_still_owns_it) {
    wipe("morton:mm2");
    ClusterRegistry& registry = admin("morton:mm2");
    registry.heartbeat_shard(shard("world-a", 9101, 10, 500), 60000);
    registry.heartbeat_shard(shard("world-b", 9102, 400, 500), 60000);

    Matchmaker matchmaker;
    CHECK(matchmaker.start(config_for("morton:mm2")));

    SessionGrant first = matchmaker.assign("player-sticky");
    CHECK(first.ok);
    CHECK(!first.reconnect);
    CHECK(first.shard_id == "world-a");

    registry.heartbeat_shard(shard("world-a", 9101, 499, 500), 60000);

    SessionGrant again = matchmaker.assign("player-sticky");
    CHECK(again.ok);
    CHECK(again.reconnect);
    CHECK(again.shard_id == "world-a");
    CHECK_EQ(again.session_token, first.session_token);
    CHECK_EQ(again.region, first.region);
    CHECK_EQ(matchmaker.grants(), 1u);
    CHECK_EQ(matchmaker.reconnects(), 1u);

    matchmaker.stop();
}

TEST_CASE(a_player_on_a_dead_shard_is_reassigned_with_a_fresh_session) {
    wipe("morton:mm3");
    ClusterRegistry& registry = admin("morton:mm3");
    registry.heartbeat_shard(shard("world-a", 9201, 10, 500), 400);
    registry.heartbeat_shard(shard("world-b", 9202, 20, 500), 60000);

    Matchmaker matchmaker;
    CHECK(matchmaker.start(config_for("morton:mm3")));

    SessionGrant first = matchmaker.assign("player-orphan");
    CHECK(first.ok);
    CHECK(first.shard_id == "world-a");

    sleep_us(700000);

    SessionGrant rescued = matchmaker.assign("player-orphan");
    CHECK(rescued.ok);
    CHECK(rescued.shard_id == "world-b");
    CHECK(rescued.session_token != first.session_token);
    CHECK(!rescued.reconnect);

    PresenceRecord record;
    CHECK(registry.get_presence("player-orphan", &record));
    CHECK(record.shard_id == "world-b");

    CHECK_EQ(matchmaker.shards().size(), 1u);

    matchmaker.stop();
}

TEST_CASE(the_http_api_serves_sessions_shards_and_releases) {
    wipe("morton:mm4");
    ClusterRegistry& registry = admin("morton:mm4");
    registry.heartbeat_shard(shard("world-a", 9301, 40, 500), 60000);
    registry.heartbeat_shard(shard("world-b", 9302, 300, 500), 60000);

    Matchmaker matchmaker;
    CHECK(matchmaker.start(config_for("morton:mm4")));
    Address api = matchmaker.http_address();
    CHECK(api.port != 0);

    std::string health = http_request(api, "GET", "/health");
    CHECK(health.find("HTTP/1.1 200 OK") == 0);
    CHECK(json_field(http_body(health), "ok") == "true");

    std::string shards = http_body(http_request(api, "GET", "/shards"));
    CHECK(json_field(shards, "count") == "2");
    CHECK(shards.find("\"id\":\"world-a\"") != std::string::npos);
    CHECK(shards.find("\"load\":0.080") != std::string::npos);
    CHECK(shards.find("\"tick_p99_ms\":3.250") != std::string::npos);

    std::string session = http_body(http_request(api, "POST", "/session?player=alice"));
    CHECK(json_field(session, "ok") == "true");
    CHECK(json_field(session, "shard") == "world-a");
    CHECK(json_field(session, "endpoint") == "127.0.0.1:9301");
    std::string token = json_field(session, "token");
    CHECK(!token.empty());

    std::string again = http_body(http_request(api, "GET", "/session?player=alice"));
    CHECK(json_field(again, "reconnect") == "true");
    CHECK(json_field(again, "token") == token);

    std::string bad = http_request(api, "POST", "/session?player=");
    CHECK(bad.find("HTTP/1.1 400 Bad Request") == 0);

    std::string released = http_request(api, "DELETE", "/session?player=alice");
    CHECK(released.find("HTTP/1.1 200 OK") == 0);

    std::string reassigned = http_body(http_request(api, "POST", "/session?player=alice"));
    CHECK(json_field(reassigned, "reconnect") == "false");
    CHECK(json_field(reassigned, "token") != token);

    matchmaker.stop();
}

TEST_CASE(concurrent_assignment_never_hands_one_player_to_two_shards) {
    wipe("morton:mm5");
    ClusterRegistry& registry = admin("morton:mm5");
    registry.heartbeat_shard(shard("world-a", 9401, 0, 5000), 60000);
    registry.heartbeat_shard(shard("world-b", 9402, 0, 5000), 60000);

    Matchmaker matchmaker;
    CHECK(matchmaker.start(config_for("morton:mm5")));
    Address api = matchmaker.http_address();

    constexpr int kThreads = 8;
    std::vector<std::thread> threads;
    std::vector<std::vector<std::string>> results(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 6; ++i) {
                std::string body =
                    http_body(http_request(api, "POST", "/session?player=contended"));
                results[static_cast<std::size_t>(t)].push_back(json_field(body, "shard") + "/" +
                                                               json_field(body, "token"));
            }
        });
    }
    for (auto& thread : threads) thread.join();

    std::string first;
    int mismatches = 0;
    for (const auto& thread_results : results) {
        for (const std::string& value : thread_results) {
            CHECK(!value.empty());
            if (first.empty()) first = value;
            if (value != first) ++mismatches;
        }
    }
    CHECK_EQ(mismatches, 0);
    CHECK_EQ(matchmaker.grants(), 1u);

    matchmaker.stop();
}

int main() {
    if (!redis_fixture().start()) {
        std::printf("redis-server unavailable; skipping matchmaker integration tests\n");
        return 0;
    }
    int result = ::morton_test::run_all();
    redis_fixture().stop();
    return result;
}
