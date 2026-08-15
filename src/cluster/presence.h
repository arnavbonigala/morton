#pragma once
#include <string>
#include <vector>

#include "cluster/redis.h"
#include "core/types.h"

namespace morton {

struct ShardInfo {
    std::string id;
    std::string udp_endpoint;
    std::string http_endpoint;
    u32 player_count = 0;
    u32 capacity = 0;
    f64 tick_p99_ms = 0.0;
    u64 heartbeat_ms = 0;
    bool draining = false;

    f64 load_factor() const {
        return capacity == 0 ? 1.0 : static_cast<f64>(player_count) / static_cast<f64>(capacity);
    }
    bool accepting() const { return !draining && player_count < capacity; }
};

struct PresenceRecord {
    std::string player_id;
    std::string shard_id;
    u32 region = 0;
    u64 session_token = 0;
    u64 updated_ms = 0;
};

/// Redis-backed shard registry and player presence directory.
///
/// Liveness is TTL-driven rather than heartbeat-scanned: a shard that stops
/// heartbeating simply expires, so a killed process is indistinguishable from
/// a cleanly stopped one and no separate reaper has to run. Membership
/// entries left behind by expired shards are pruned during discovery.
class ClusterRegistry {
public:
    bool connect(const Address& redis, const std::string& key_prefix = "morton");
    void disconnect() { redis_.disconnect(); }
    bool connected() const { return redis_.connected(); }
    RedisClient& client() { return redis_; }
    const std::string& prefix() const { return prefix_; }

    bool heartbeat_shard(const ShardInfo& shard, u32 ttl_ms);
    bool remove_shard(const std::string& shard_id);

    /// Live shards, with expired membership entries pruned as a side effect.
    std::vector<ShardInfo> live_shards(u32* pruned = nullptr);
    bool get_shard(const std::string& shard_id, ShardInfo* out);

    bool set_presence(const PresenceRecord& record, u32 ttl_ms);
    bool get_presence(const std::string& player_id, PresenceRecord* out);
    bool clear_presence(const std::string& player_id);

    /// Atomically moves a player's presence from one shard to another, but only
    /// if the record still names `from_shard`. A migration that races a
    /// reconnect or a second handoff loses instead of duplicating the player.
    bool transfer_presence(const std::string& player_id, const std::string& from_shard,
                           const std::string& to_shard, u32 region, u32 ttl_ms);

    /// Queues the transfer so it can share a round trip with the caller's other
    /// commands. The reply is a 1 when the transfer took.
    void queue_transfer_presence(const std::string& player_id, const std::string& from_shard,
                                 const std::string& to_shard, u32 region, u32 ttl_ms);

    /// Claims exclusive ownership of a player id for `shard_id`. Fails if a
    /// different live shard already owns it.
    bool claim_presence(const PresenceRecord& record, u32 ttl_ms, std::string* current_owner);

    std::vector<std::string> players_on_shard(const std::string& shard_id);

private:
    std::string shard_key(const std::string& id) const { return prefix_ + ":shard:" + id; }
    std::string presence_key(const std::string& id) const { return prefix_ + ":presence:" + id; }
    std::string members_key() const { return prefix_ + ":shards"; }
    std::string roster_key(const std::string& id) const { return prefix_ + ":roster:" + id; }

    RedisClient redis_;
    std::string prefix_ = "morton";
};

}  // namespace morton
