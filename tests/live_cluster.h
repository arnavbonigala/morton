#pragma once
#include <atomic>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "app/world_server.h"
#include "cluster/matchmaker.h"
#include "core/time.h"
#include "redis_fixture.h"

namespace morton_test {

using morton::Address;
using morton::Matchmaker;
using morton::MatchmakerConfig;
using morton::RedisClient;
using morton::RedisReply;
using morton::RedisType;
using morton::RegionMap;
using morton::u32;
using morton::wall_ms;
using morton::WorldServer;
using morton::WorldServerConfig;

RegionMap test_regions() {
    RegionMap map;
    map.world_size = 2048.f;
    map.regions_per_axis = 2;
    map.margin = 48.f;
    return map;
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

/// A cluster whose shards tick on their own threads, so the fleet under test
/// talks to servers that are genuinely running rather than being pumped by the
/// test loop.
struct LiveCluster {
    std::vector<std::unique_ptr<WorldServer>> servers;
    std::vector<std::thread> threads;
    std::set<std::size_t> dead;
    Matchmaker matchmaker;

    bool start(const std::string& prefix, const std::vector<std::string>& shard_ids, u32 drifters) {
        for (const std::string& id : shard_ids) {
            WorldServerConfig config;
            config.shard_id = id;
            config.udp_bind = Address(0x7f000001u, 0);
            config.http_bind = Address(0x7f000001u, 0);
            config.redis = redis_fixture().address();
            config.key_prefix = prefix;
            config.regions = test_regions();
            config.capacity = 4096;
            config.max_connections = 8192;
            config.drifters = drifters;
            auto server = std::make_unique<WorldServer>();
            if (!server->start(config)) return false;
            servers.push_back(std::move(server));
        }

        MatchmakerConfig config;
        config.redis = redis_fixture().address();
        config.http_bind = Address(0x7f000001u, 0);
        config.key_prefix = prefix;
        config.session_ttl_ms = 60000;
        config.regions = test_regions();
        if (!matchmaker.start(config)) return false;

        for (auto& server : servers) {
            server->coordinator().refresh(wall_ms(), 0, 0.0, true);
        }
        for (auto& server : servers) {
            WorldServer* raw = server.get();
            threads.emplace_back([raw] { raw->run(); });
        }
        return true;
    }

    void kill(std::size_t index) {
        servers[index]->request_stop();
        threads[index].join();
        servers[index]->stop();
        dead.insert(index);
    }

    void shutdown() {
        for (std::size_t i = 0; i < servers.size(); ++i) {
            if (dead.count(i) == 0) servers[i]->request_stop();
        }
        for (std::size_t i = 0; i < threads.size(); ++i) {
            if (dead.count(i) == 0) threads[i].join();
        }
        threads.clear();
        matchmaker.stop();
        for (std::size_t i = 0; i < servers.size(); ++i) {
            if (dead.count(i) == 0) servers[i]->stop();
        }
        servers.clear();
    }

    u32 residents() const {
        u32 total = 0;
        for (const auto& server : servers) total += server->resident_player_count();
        return total;
    }
};

}  // namespace morton_test
