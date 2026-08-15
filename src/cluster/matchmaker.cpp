#include "cluster/matchmaker.h"

#include <algorithm>
#include <cstdio>

#include "core/log.h"
#include "core/time.h"
#include "metrics/registry.h"

namespace morton {
namespace {

std::string number(f64 value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.3f", value);
    return buffer;
}

}  // namespace

std::string session_grant_json(const SessionGrant& grant) {
    if (!grant.ok) {
        return "{\"ok\":false,\"reason\":\"" + json_escape(grant.reason) + "\"}";
    }
    return "{\"ok\":true,\"reconnect\":" + std::string(grant.reconnect ? "true" : "false") +
           ",\"player\":\"" + json_escape(grant.player_id) + "\",\"shard\":\"" +
           json_escape(grant.shard_id) + "\",\"endpoint\":\"" + json_escape(grant.udp_endpoint) +
           "\",\"region\":" + std::to_string(grant.region) + ",\"token\":\"" +
           std::to_string(grant.session_token) + "\"}";
}

bool Matchmaker::start(const MatchmakerConfig& config) {
    config_ = config;
    rng_.seed(static_cast<u64>(now_us()) ^ (static_cast<u64>(wall_ms()) << 21));

    if (!registry_.connect(config_.redis, config_.key_prefix)) {
        MORTON_LOG_ERROR("matchmaker cannot reach redis at %s", config_.redis.to_string().c_str());
        return false;
    }

    http_.route("GET", "/health", [this](const HttpRequest&) {
        return HttpResponse::json("{\"ok\":" +
                                  std::string(registry_.connected() ? "true" : "false") + "}");
    });

    http_.route("GET", "/shards", [this](const HttpRequest&) {
        std::vector<ShardInfo> live = shards();
        return HttpResponse::json(shards_json(live));
    });

    http_.route("GET", "/metrics", [](const HttpRequest&) {
        return HttpResponse::text(MetricsRegistry::instance().expose());
    });

    auto session_handler = [this](const HttpRequest& request) {
        std::string player = request.query_value("player");
        if (player.empty() || player.size() > 64) {
            return HttpResponse::bad_request("player id must be 1-64 characters");
        }
        SessionGrant grant = assign(player);
        HttpResponse response = HttpResponse::json(session_grant_json(grant));
        if (!grant.ok) response.status = 503;
        return response;
    };
    http_.route("POST", "/session", session_handler);
    http_.route("GET", "/session", session_handler);

    http_.route("DELETE", "/session", [this](const HttpRequest& request) {
        std::string player = request.query_value("player");
        if (player.empty()) return HttpResponse::bad_request("player id required");
        release(player);
        return HttpResponse::json("{\"ok\":true}");
    });

    if (!http_.start(config_.http_bind)) return false;

    MORTON_LOG_INFO("matchmaker listening on %s", http_.local_address().to_string().c_str());
    return true;
}

void Matchmaker::stop() {
    http_.stop();
    registry_.disconnect();
}

std::vector<ShardInfo> Matchmaker::shards() {
    std::lock_guard<std::mutex> guard(mutex_);
    return registry_.live_shards();
}

ShardInfo* Matchmaker::pick_shard(std::vector<ShardInfo>& live) {
    ShardInfo* best = nullptr;
    for (ShardInfo& shard : live) {
        if (!shard.accepting()) continue;
        if (best == nullptr) {
            best = &shard;
            continue;
        }
        f64 delta = shard.load_factor() - best->load_factor();
        if (delta < -0.0001) {
            best = &shard;
        } else if (delta <= 0.0001 && shard.id < best->id) {
            best = &shard;
        }
    }
    return best;
}

SessionGrant Matchmaker::assign(const std::string& player_id) {
    std::lock_guard<std::mutex> guard(mutex_);

    SessionGrant grant;
    grant.player_id = player_id;

    std::vector<ShardInfo> live = registry_.live_shards();
    if (live.empty()) {
        ++rejections_;
        grant.reason = "no live shards";
        return grant;
    }

    auto live_shard = [&live](const std::string& id) -> const ShardInfo* {
        for (const ShardInfo& shard : live) {
            if (shard.id == id) return &shard;
        }
        return nullptr;
    };

    PresenceRecord existing;
    if (registry_.get_presence(player_id, &existing) && !existing.shard_id.empty()) {
        const ShardInfo* owner = live_shard(existing.shard_id);
        if (owner != nullptr && !owner->draining) {
            grant.ok = true;
            grant.reconnect = true;
            grant.shard_id = owner->id;
            grant.udp_endpoint = owner->udp_endpoint;
            grant.region = existing.region;
            grant.session_token = existing.session_token;
            registry_.set_presence(existing, config_.session_ttl_ms);
            ++reconnects_;
            return grant;
        }
        // The shard holding this player is gone or draining; the stale record
        // would otherwise block every future claim for this id forever.
        registry_.clear_presence(player_id);
    }

    ShardInfo* chosen = pick_shard(live);
    if (chosen == nullptr) {
        ++rejections_;
        grant.reason = "all shards full or draining";
        return grant;
    }

    PresenceRecord record;
    record.player_id = player_id;
    record.shard_id = chosen->id;
    record.session_token = rng_();
    if (record.session_token == 0) record.session_token = 1;

    std::vector<std::string> ids;
    ids.reserve(live.size());
    for (const ShardInfo& shard : live) {
        if (!shard.draining) ids.push_back(shard.id);
    }
    std::sort(ids.begin(), ids.end());
    std::vector<std::string> owners = assign_regions(ids, config_.regions.region_count());
    for (u32 region = 0; region < owners.size(); ++region) {
        if (owners[region] == chosen->id) {
            record.region = region;
            break;
        }
    }

    std::string current_owner;
    if (!registry_.claim_presence(record, config_.session_ttl_ms, &current_owner)) {
        const ShardInfo* winner = live_shard(current_owner);
        if (winner == nullptr) {
            ++rejections_;
            grant.reason = "presence claim lost";
            return grant;
        }
        PresenceRecord owned;
        registry_.get_presence(player_id, &owned);
        grant.ok = true;
        grant.reconnect = true;
        grant.shard_id = winner->id;
        grant.udp_endpoint = winner->udp_endpoint;
        grant.region = owned.region;
        grant.session_token = owned.session_token;
        ++reconnects_;
        return grant;
    }

    grant.ok = true;
    grant.shard_id = chosen->id;
    grant.udp_endpoint = chosen->udp_endpoint;
    grant.region = record.region;
    grant.session_token = record.session_token;
    ++grants_;
    return grant;
}

bool Matchmaker::release(const std::string& player_id) {
    std::lock_guard<std::mutex> guard(mutex_);
    return registry_.clear_presence(player_id);
}

std::string Matchmaker::shards_json(const std::vector<ShardInfo>& live) const {
    std::string out = "{\"shards\":[";
    for (std::size_t i = 0; i < live.size(); ++i) {
        const ShardInfo& shard = live[i];
        if (i > 0) out += ",";
        out += "{\"id\":\"" + json_escape(shard.id) + "\",\"udp\":\"" +
               json_escape(shard.udp_endpoint) + "\",\"http\":\"" +
               json_escape(shard.http_endpoint) + "\",\"players\":" +
               std::to_string(shard.player_count) + ",\"capacity\":" +
               std::to_string(shard.capacity) + ",\"load\":" + number(shard.load_factor()) +
               ",\"tick_p99_ms\":" + number(shard.tick_p99_ms) + ",\"draining\":" +
               (shard.draining ? "true" : "false") + "}";
    }
    out += "],\"count\":" + std::to_string(live.size()) + "}";
    return out;
}

}  // namespace morton
