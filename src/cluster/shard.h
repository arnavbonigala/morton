#pragma once
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "cluster/migration.h"
#include "cluster/presence.h"
#include "core/types.h"
#include "sim/region.h"

namespace morton {

struct ShardConfig {
    std::string id;
    std::string udp_endpoint;
    std::string http_endpoint;
    u32 capacity = 512;
    u32 heartbeat_ttl_ms = 4000;
    u32 refresh_interval_ms = 1000;
    u32 presence_ttl_ms = 15000;
    u32 ticket_ttl_ms = 10000;
    RegionMap regions;
};

struct HandoffPlan {
    u64 token = 0;
    u32 target_region = kInvalidRegion;
    std::string target_shard;
    std::string target_endpoint;
};

/// Region ownership, membership refresh and live player handoff for one shard.
///
/// Ownership is derived, not elected: every shard maps regions onto the sorted
/// live-shard list from the same registry snapshot, so all of them agree without
/// a consensus round, and a dead shard's regions are simply reassigned by the
/// next refresh.
class ShardCoordinator {
public:
    bool start(const ShardConfig& config, const Address& redis,
               const std::string& key_prefix = "morton");
    void stop();

    /// Heartbeats and re-reads membership when the refresh interval has elapsed.
    /// Returns true if the ownership map changed.
    bool refresh(u64 now_ms, u32 player_count, f64 tick_p99_ms, bool force = false);

    bool owns_region(u32 region) const;
    std::vector<u32> owned_regions() const;
    const std::string& owner_of_region(u32 region) const;
    bool endpoint_of_shard(const std::string& shard_id, std::string* endpoint) const;

    /// Region a spawning player should be placed in, preferring an owned one.
    u32 spawn_region() const;

    /// Decides whether a player standing at (x, y) belongs to another shard, and
    /// if so publishes a handoff ticket and moves presence over in one step.
    /// A handoff is only started when the destination shard is actually live.
    bool plan_handoff(const std::string& player_id, u32 current_region, const Vec2& position,
                      const Vec2& velocity, Tick tick, u32 last_input_sequence,
                      HandoffPlan* plan);

    /// Redeems an inbound ticket. Fails for replayed, expired or foreign tickets.
    bool accept_handoff(u64 token, MigrationTicket* ticket);

    bool touch_presence(const PresenceRecord& record);

    /// Heartbeats the whole resident roster in one round trip rather than one
    /// per player, which is otherwise the longest thing a tick does.
    void queue_touch_presence(const PresenceRecord& record);
    bool flush_presence();

    bool claim_player(const PresenceRecord& record, std::string* current_owner);
    bool release_player(const std::string& player_id);

    const ShardConfig& config() const { return config_; }
    const std::vector<ShardInfo>& members() const { return members_; }
    ClusterRegistry& registry() { return registry_; }
    MigrationStore& tickets() { return tickets_; }

    u64 handoffs_started() const { return handoffs_started_; }
    u64 handoffs_accepted() const { return handoffs_accepted_; }
    u64 handoffs_declined() const { return handoffs_declined_; }
    u32 shards_pruned() const { return shards_pruned_; }

private:
    void recompute_ownership();
    u64 next_token();

    ShardConfig config_;
    ClusterRegistry registry_;
    MigrationStore tickets_{&registry_};
    std::vector<ShardInfo> members_;
    std::vector<std::string> region_owner_;
    std::unordered_map<std::string, std::string> endpoints_;
    u64 last_refresh_ms_ = 0;
    std::mt19937_64 rng_;
    u64 handoffs_started_ = 0;
    u64 handoffs_accepted_ = 0;
    u64 handoffs_declined_ = 0;
    u32 shards_pruned_ = 0;
};

}  // namespace morton
