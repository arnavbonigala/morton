#include "cluster/presence.h"

#include <cstdlib>

#include "core/log.h"
#include "core/time.h"

namespace morton {
namespace {

const char* kTransferScript = R"(
local current = redis.call('HGET', KEYS[1], 'shard')
if current and current ~= ARGV[1] then return 0 end
redis.call('HSET', KEYS[1], 'shard', ARGV[2], 'region', ARGV[3], 'updated', ARGV[4])
redis.call('PEXPIRE', KEYS[1], ARGV[5])
redis.call('SREM', KEYS[2], ARGV[6])
redis.call('SADD', KEYS[3], ARGV[6])
redis.call('PEXPIRE', KEYS[3], ARGV[5])
return 1
)";

const char* kClaimScript = R"(
local current = redis.call('HGET', KEYS[1], 'shard')
if current and current ~= ARGV[1] then return current end
redis.call('HSET', KEYS[1], 'shard', ARGV[1], 'region', ARGV[2], 'session', ARGV[3], 'updated', ARGV[4])
redis.call('PEXPIRE', KEYS[1], ARGV[5])
redis.call('SADD', KEYS[2], ARGV[6])
redis.call('PEXPIRE', KEYS[2], ARGV[5])
return 1
)";

std::string field(const RedisReply& hash, const char* name) {
    for (std::size_t i = 0; i + 1 < hash.elements.size(); i += 2) {
        if (hash.elements[i].str == name) return hash.elements[i + 1].str;
    }
    return "";
}

u64 field_u64(const RedisReply& hash, const char* name) {
    std::string value = field(hash, name);
    return value.empty() ? 0 : std::strtoull(value.c_str(), nullptr, 10);
}

f64 field_f64(const RedisReply& hash, const char* name) {
    std::string value = field(hash, name);
    return value.empty() ? 0.0 : std::strtod(value.c_str(), nullptr);
}

ShardInfo parse_shard(const std::string& id, const RedisReply& hash) {
    ShardInfo shard;
    shard.id = id;
    shard.udp_endpoint = field(hash, "udp");
    shard.http_endpoint = field(hash, "http");
    shard.player_count = static_cast<u32>(field_u64(hash, "players"));
    shard.capacity = static_cast<u32>(field_u64(hash, "capacity"));
    shard.tick_p99_ms = field_f64(hash, "tick_p99_ms");
    shard.heartbeat_ms = field_u64(hash, "heartbeat");
    shard.draining = field(hash, "draining") == "1";
    return shard;
}

}  // namespace

bool ClusterRegistry::connect(const Address& redis, const std::string& key_prefix) {
    prefix_ = key_prefix;
    return redis_.connect(redis);
}

bool ClusterRegistry::heartbeat_shard(const ShardInfo& shard, u32 ttl_ms) {
    if (shard.id.empty()) return false;
    const std::string key = shard_key(shard.id);
    const std::string ttl = std::to_string(ttl_ms);

    redis_.queue({"HSET", key, "udp", shard.udp_endpoint, "http", shard.http_endpoint, "players",
                  std::to_string(shard.player_count), "capacity", std::to_string(shard.capacity),
                  "tick_p99_ms", std::to_string(shard.tick_p99_ms), "heartbeat",
                  std::to_string(wall_ms()), "draining", shard.draining ? "1" : "0"});
    redis_.queue({"PEXPIRE", key, ttl});
    redis_.queue({"SADD", members_key(), shard.id});

    std::vector<RedisReply> replies;
    if (!redis_.flush(&replies)) return false;
    for (const RedisReply& reply : replies) {
        if (reply.is_error()) {
            MORTON_LOG_WARN("redis heartbeat error: %s", reply.str.c_str());
            return false;
        }
    }
    return true;
}

bool ClusterRegistry::remove_shard(const std::string& shard_id) {
    redis_.queue({"DEL", shard_key(shard_id)});
    redis_.queue({"SREM", members_key(), shard_id});
    redis_.queue({"DEL", roster_key(shard_id)});
    return redis_.flush(nullptr);
}

std::vector<ShardInfo> ClusterRegistry::live_shards(u32* pruned) {
    if (pruned) *pruned = 0;

    RedisReply members = redis_.command({"SMEMBERS", members_key()});
    if (members.type != RedisType::kArray || members.elements.empty()) return {};

    std::vector<std::string> ids;
    ids.reserve(members.elements.size());
    for (const RedisReply& member : members.elements) ids.push_back(member.str);

    for (const std::string& id : ids) redis_.queue({"HGETALL", shard_key(id)});

    std::vector<RedisReply> hashes;
    if (!redis_.flush(&hashes) || hashes.size() != ids.size()) return {};

    std::vector<ShardInfo> live;
    std::vector<std::string> dead;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (hashes[i].type != RedisType::kArray || hashes[i].elements.empty()) {
            dead.push_back(ids[i]);
            continue;
        }
        live.push_back(parse_shard(ids[i], hashes[i]));
    }

    if (!dead.empty()) {
        for (const std::string& id : dead) {
            redis_.queue({"SREM", members_key(), id});
            redis_.queue({"DEL", roster_key(id)});
        }
        redis_.flush(nullptr);
        if (pruned) *pruned = static_cast<u32>(dead.size());
        MORTON_LOG_INFO("pruned %zu expired shard registrations", dead.size());
    }

    return live;
}

bool ClusterRegistry::get_shard(const std::string& shard_id, ShardInfo* out) {
    RedisReply hash = redis_.command({"HGETALL", shard_key(shard_id)});
    if (hash.type != RedisType::kArray || hash.elements.empty()) return false;
    *out = parse_shard(shard_id, hash);
    return true;
}

void ClusterRegistry::queue_set_presence(const PresenceRecord& record, u32 ttl_ms) {
    const std::string key = presence_key(record.player_id);
    const std::string ttl = std::to_string(ttl_ms);

    redis_.queue({"HSET", key, "shard", record.shard_id, "region", std::to_string(record.region),
                  "session", std::to_string(record.session_token), "updated",
                  std::to_string(wall_ms())});
    redis_.queue({"PEXPIRE", key, ttl});
    redis_.queue({"SADD", roster_key(record.shard_id), record.player_id});
    redis_.queue({"PEXPIRE", roster_key(record.shard_id), ttl});
}

bool ClusterRegistry::set_presence(const PresenceRecord& record, u32 ttl_ms) {
    queue_set_presence(record, ttl_ms);

    std::vector<RedisReply> replies;
    if (!redis_.flush(&replies)) return false;
    for (const RedisReply& reply : replies) {
        if (reply.is_error()) return false;
    }
    return true;
}

bool ClusterRegistry::get_presence(const std::string& player_id, PresenceRecord* out) {
    RedisReply hash = redis_.command({"HGETALL", presence_key(player_id)});
    if (hash.type != RedisType::kArray || hash.elements.empty()) return false;
    out->player_id = player_id;
    out->shard_id = field(hash, "shard");
    out->region = static_cast<u32>(field_u64(hash, "region"));
    out->session_token = field_u64(hash, "session");
    out->updated_ms = field_u64(hash, "updated");
    return true;
}

bool ClusterRegistry::clear_presence(const std::string& player_id) {
    PresenceRecord record;
    if (get_presence(player_id, &record) && !record.shard_id.empty()) {
        redis_.queue({"SREM", roster_key(record.shard_id), player_id});
    }
    redis_.queue({"DEL", presence_key(player_id)});
    return redis_.flush(nullptr);
}

void ClusterRegistry::queue_transfer_presence(const std::string& player_id,
                                              const std::string& from_shard,
                                              const std::string& to_shard, u32 region,
                                              u32 ttl_ms) {
    redis_.queue({"EVAL", kTransferScript, "3", presence_key(player_id), roster_key(from_shard),
                  roster_key(to_shard), from_shard, to_shard, std::to_string(region),
                  std::to_string(wall_ms()), std::to_string(ttl_ms), player_id});
}

bool ClusterRegistry::transfer_presence(const std::string& player_id,
                                        const std::string& from_shard,
                                        const std::string& to_shard, u32 region, u32 ttl_ms) {
    queue_transfer_presence(player_id, from_shard, to_shard, region, ttl_ms);
    std::vector<RedisReply> replies;
    if (!redis_.flush(&replies) || replies.empty()) return false;
    const RedisReply& reply = replies.front();
    if (reply.is_error()) {
        MORTON_LOG_WARN("presence transfer failed: %s", reply.str.c_str());
        return false;
    }
    return reply.type == RedisType::kInteger && reply.integer == 1;
}

bool ClusterRegistry::claim_presence(const PresenceRecord& record, u32 ttl_ms,
                                     std::string* current_owner) {
    RedisReply reply = redis_.command(
        {"EVAL", kClaimScript, "2", presence_key(record.player_id), roster_key(record.shard_id),
         record.shard_id, std::to_string(record.region), std::to_string(record.session_token),
         std::to_string(wall_ms()), std::to_string(ttl_ms), record.player_id});

    if (reply.is_error()) {
        MORTON_LOG_WARN("presence claim failed: %s", reply.str.c_str());
        return false;
    }
    if (reply.type == RedisType::kInteger && reply.integer == 1) return true;
    if (current_owner) *current_owner = reply.str;
    return false;
}

std::vector<std::string> ClusterRegistry::players_on_shard(const std::string& shard_id) {
    RedisReply members = redis_.command({"SMEMBERS", roster_key(shard_id)});
    std::vector<std::string> players;
    if (members.type != RedisType::kArray) return players;
    players.reserve(members.elements.size());
    for (const RedisReply& member : members.elements) players.push_back(member.str);
    return players;
}

}  // namespace morton
