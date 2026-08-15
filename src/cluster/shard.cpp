#include "cluster/shard.h"

#include <algorithm>

#include "core/log.h"
#include "core/time.h"

namespace morton {
namespace {

const std::string kNoOwner;

}  // namespace

bool ShardCoordinator::start(const ShardConfig& config, const Address& redis,
                             const std::string& key_prefix) {
    config_ = config;
    region_owner_.assign(config_.regions.region_count(), std::string());

    u64 seed = wall_ms();
    for (char c : config_.id) seed = seed * 1099511628211ull + static_cast<u64>(c);
    seed ^= static_cast<u64>(now_us());
    rng_.seed(seed);

    if (!registry_.connect(redis, key_prefix)) {
        MORTON_LOG_ERROR("shard %s cannot reach redis at %s", config_.id.c_str(),
                         redis.to_string().c_str());
        return false;
    }

    return refresh(wall_ms(), 0, 0.0, true);
}

void ShardCoordinator::stop() {
    if (!registry_.connected()) return;
    registry_.remove_shard(config_.id);
    registry_.disconnect();
}

bool ShardCoordinator::refresh(u64 now_ms, u32 player_count, f64 tick_p99_ms, bool force) {
    if (!force && now_ms - last_refresh_ms_ < config_.refresh_interval_ms) return false;
    last_refresh_ms_ = now_ms;

    ShardInfo self;
    self.id = config_.id;
    self.udp_endpoint = config_.udp_endpoint;
    self.http_endpoint = config_.http_endpoint;
    self.player_count = player_count;
    self.capacity = config_.capacity;
    self.tick_p99_ms = tick_p99_ms;

    if (!registry_.heartbeat_shard(self, config_.heartbeat_ttl_ms)) {
        MORTON_LOG_WARN("shard %s heartbeat failed", config_.id.c_str());
        return false;
    }

    u32 pruned = 0;
    std::vector<ShardInfo> members = registry_.live_shards(&pruned);
    shards_pruned_ += pruned;
    if (members.empty()) return false;

    std::vector<std::string> previous = region_owner_;
    members_ = std::move(members);
    recompute_ownership();

    return region_owner_ != previous;
}

void ShardCoordinator::recompute_ownership() {
    std::vector<std::string> ids;
    ids.reserve(members_.size());
    endpoints_.clear();
    for (const ShardInfo& shard : members_) {
        endpoints_[shard.id] = shard.udp_endpoint;
        if (shard.draining && shard.id != config_.id) continue;
        ids.push_back(shard.id);
    }
    std::sort(ids.begin(), ids.end());

    region_owner_ = assign_regions(ids, config_.regions.region_count());
}

bool ShardCoordinator::owns_region(u32 region) const {
    return region < region_owner_.size() && region_owner_[region] == config_.id;
}

std::vector<u32> ShardCoordinator::owned_regions() const {
    std::vector<u32> owned;
    for (u32 region = 0; region < region_owner_.size(); ++region) {
        if (region_owner_[region] == config_.id) owned.push_back(region);
    }
    return owned;
}

const std::string& ShardCoordinator::owner_of_region(u32 region) const {
    return region < region_owner_.size() ? region_owner_[region] : kNoOwner;
}

bool ShardCoordinator::endpoint_of_shard(const std::string& shard_id, std::string* endpoint) const {
    auto it = endpoints_.find(shard_id);
    if (it == endpoints_.end() || it->second.empty()) return false;
    *endpoint = it->second;
    return true;
}

u32 ShardCoordinator::spawn_region() const {
    std::vector<u32> owned = owned_regions();
    if (owned.empty()) return 0;
    return owned[0];
}

u64 ShardCoordinator::next_token() {
    u64 token = rng_();
    return token == 0 ? 1 : token;
}

bool ShardCoordinator::plan_handoff(const std::string& player_id, u32 current_region,
                                    const Vec2& position, const Vec2& velocity, Tick tick,
                                    u32 last_input_sequence, HandoffPlan* plan) {
    u32 target_region = config_.regions.handoff_target(current_region, position.x, position.y);
    if (target_region == kInvalidRegion) return false;

    const std::string& owner = owner_of_region(target_region);
    if (owner.empty() || owner == config_.id) return false;

    std::string endpoint;
    if (!endpoint_of_shard(owner, &endpoint)) {
        ++handoffs_declined_;
        return false;
    }

    MigrationTicket ticket;
    ticket.token = next_token();
    ticket.player_id = player_id;
    ticket.from_shard = config_.id;
    ticket.to_shard = owner;
    ticket.region = target_region;
    ticket.tick = tick;
    ticket.position = position;
    ticket.velocity = velocity;
    ticket.last_input_sequence = last_input_sequence;

    if (!tickets_.publish_and_transfer(ticket, config_.ticket_ttl_ms, config_.presence_ttl_ms)) {
        ++handoffs_declined_;
        return false;
    }

    plan->token = ticket.token;
    plan->target_region = target_region;
    plan->target_shard = owner;
    plan->target_endpoint = endpoint;
    ++handoffs_started_;
    return true;
}

bool ShardCoordinator::accept_handoff(u64 token, MigrationTicket* ticket) {
    if (!tickets_.redeem(token, ticket)) return false;
    if (ticket->to_shard != config_.id) {
        MORTON_LOG_WARN("shard %s got a ticket addressed to %s", config_.id.c_str(),
                        ticket->to_shard.c_str());
        return false;
    }
    ++handoffs_accepted_;
    return true;
}

bool ShardCoordinator::touch_presence(const PresenceRecord& record) {
    return registry_.set_presence(record, config_.presence_ttl_ms);
}

void ShardCoordinator::queue_touch_presence(const PresenceRecord& record) {
    registry_.queue_set_presence(record, config_.presence_ttl_ms);
}

bool ShardCoordinator::flush_presence() {
    std::vector<RedisReply> replies;
    if (!registry_.client().flush(&replies)) return false;
    for (const RedisReply& reply : replies) {
        if (reply.is_error()) return false;
    }
    return true;
}

bool ShardCoordinator::claim_player(const PresenceRecord& record, std::string* current_owner) {
    return registry_.claim_presence(record, config_.presence_ttl_ms, current_owner);
}

bool ShardCoordinator::release_player(const std::string& player_id) {
    return registry_.clear_presence(player_id);
}

}  // namespace morton
