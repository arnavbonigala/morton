#pragma once
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include "cluster/presence.h"
#include "core/types.h"
#include "net/http.h"
#include "sim/region.h"

namespace morton {

struct MatchmakerConfig {
    Address redis;
    Address http_bind;
    std::string key_prefix = "morton";
    u32 session_ttl_ms = 15000;
    RegionMap regions;
};

struct SessionGrant {
    bool ok = false;
    bool reconnect = false;
    std::string player_id;
    std::string shard_id;
    std::string udp_endpoint;
    u32 region = 0;
    u64 session_token = 0;
    std::string reason;
};

/// Stateless-by-design matchmaker.
///
/// It stores nothing locally: every decision is derived from the Redis registry,
/// so any number of matchmakers can run behind a load balancer and a restart
/// loses nothing. Players already present are steered back to the shard that
/// still owns them, which makes a reconnect a lookup rather than a new session.
class Matchmaker {
public:
    bool start(const MatchmakerConfig& config);
    void stop();

    SessionGrant assign(const std::string& player_id);
    bool release(const std::string& player_id);

    std::vector<ShardInfo> shards();
    Address http_address() const { return http_.local_address(); }

    u64 grants() const { return grants_; }
    u64 reconnects() const { return reconnects_; }
    u64 rejections() const { return rejections_; }

private:
    ShardInfo* pick_shard(std::vector<ShardInfo>& live);
    std::string shards_json(const std::vector<ShardInfo>& live) const;

    MatchmakerConfig config_;
    ClusterRegistry registry_;
    HttpServer http_;
    std::mutex mutex_;
    std::mt19937_64 rng_;
    u64 grants_ = 0;
    u64 reconnects_ = 0;
    u64 rejections_ = 0;
};

std::string session_grant_json(const SessionGrant& grant);

}  // namespace morton
