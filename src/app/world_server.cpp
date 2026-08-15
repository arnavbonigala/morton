#include "app/world_server.h"

#include <algorithm>
#include <cstdio>

#include "core/log.h"
#include "core/time.h"
#include "net/udp_socket.h"

namespace morton {
namespace {

constexpr u32 kSnapshotChannelBytes = 1;
constexpr u64 kPendingAdmitTtlMs = 15000;
constexpr u64 kRedeemedTicketTtlMs = 30000;
constexpr u64 kPresenceRefreshMs = 2000;

std::string number(f64 value, int precision = 3) {
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%.*f", precision, value);
    return buffer;
}

}  // namespace

bool WorldServer::start(const WorldServerConfig& config) {
    config_ = config;
    world_.configure(config_.params, 64);

    MetricsRegistry& metrics = MetricsRegistry::instance();
    metrics.set_label("shard", config_.shard_id);
    tick_histogram_ = metrics.histogram("morton_tick_seconds", "simulation tick duration");
    snapshot_histogram_ =
        metrics.histogram("morton_snapshot_bytes", "per-client snapshot size in bytes");
    snapshots_counter_ = metrics.counter("morton_snapshots_total", "snapshots sent");
    snapshot_bytes_counter_ =
        metrics.counter("morton_snapshot_bytes_total", "snapshot payload bytes sent");
    inputs_counter_ = metrics.counter("morton_inputs_total", "client commands accepted");
    migrations_out_counter_ =
        metrics.counter("morton_migrations_out_total", "players handed to another shard");
    migrations_in_counter_ =
        metrics.counter("morton_migrations_in_total", "players adopted from another shard");
    players_gauge_ = metrics.gauge("morton_players", "connected players");
    entities_gauge_ = metrics.gauge("morton_entities", "simulated entities");

    ServerConfig server;
    server.bind = config_.udp_bind;
    server.max_connections = config_.max_connections;
    server.timeout_us = config_.connection_timeout_us;
    if (!connections_.start(server)) return false;

    connections_.set_admit_handler(
        [this](const Address& from, const ConnectToken& token) { return admit(from, token); });
    connections_.set_connect_handler([this](Connection& connection) { on_connect(connection); });
    connections_.set_disconnect_handler(
        [this](Connection& connection, const char* reason) { on_disconnect(connection, reason); });
    connections_.set_payload_handler(
        [this](Connection& connection, const u8* data, u32 size) {
            on_payload(connection, data, size);
        });

    http_.route("GET", "/metrics", [](const HttpRequest&) {
        return HttpResponse::text(MetricsRegistry::instance().expose());
    });
    http_.route("GET", "/health", [this](const HttpRequest&) {
        return HttpResponse::json("{\"ok\":true,\"shard\":\"" + config_.shard_id +
                                  "\",\"players\":" + std::to_string(players_.size()) + "}");
    });
    http_.route("GET", "/state", [this](const HttpRequest&) {
        return HttpResponse::json(state_json());
    });

    if (!http_.start(config_.http_bind)) {
        MORTON_LOG_WARN("shard %s could not bind its http endpoint", config_.shard_id.c_str());
    }

    // Advertised endpoints must be the bound ones: with an ephemeral port the
    // configured address is still :0, and every peer would dial nowhere.
    if (config_.advertise) {
        ShardConfig shard;
        shard.id = config_.shard_id;
        shard.capacity = config_.capacity;
        shard.regions = config_.regions;
        shard.udp_endpoint = connections_.local_address().to_string();
        shard.http_endpoint = http_.local_address().to_string();
        if (!coordinator_.start(shard, config_.redis, config_.key_prefix)) {
            http_.stop();
            connections_.stop();
            return false;
        }
    }

    for (u32 i = 0; i < config_.drifters; ++i) {
        u32 seed = hash_u32(i * 2654435761u + 17u);
        f32 x = static_cast<f32>(seed % 1000) / 1000.f * config_.params.size;
        f32 y = static_cast<f32>((seed >> 10) % 1000) / 1000.f * config_.params.size;
        world_.spawn_drifter(Vec2{x, y});
    }

    scratch_.resize(kMaxDatagramSize);
    running_.store(true, std::memory_order_relaxed);

    MORTON_LOG_INFO("shard %s simulating on %s (http %s)", config_.shard_id.c_str(),
                    connections_.local_address().to_string().c_str(),
                    http_.local_address().to_string().c_str());
    return true;
}

void WorldServer::stop() {
    running_.store(false, std::memory_order_relaxed);
    http_.stop();
    connections_.stop();
    if (config_.advertise) coordinator_.stop();
}

AdmitDecision WorldServer::admit(const Address& from, const ConnectToken& token) {
    AdmitDecision decision;
    decision.reason = DenyReason::kBadToken;

    ConnectCredential credential;
    if (!decode_credential(token, &credential)) {
        ++stats_.denied_connects;
        return decision;
    }

    u64 now_ms = wall_ms();

    auto pending = pending_by_player_.find(credential.player_id);
    if (pending != pending_by_player_.end()) {
        auto entry = pending_admits_.find(pending->second);
        if (entry != pending_admits_.end() && entry->second.expires_ms > now_ms &&
            entry->second.credential.token == credential.token) {
            decision.admit = true;
            decision.client_id = entry->second.client;
            return decision;
        }
    }

    if (players_.size() >= config_.capacity) {
        decision.reason = DenyReason::kServerFull;
        ++stats_.denied_connects;
        return decision;
    }

    PendingAdmit admitted;
    admitted.credential = credential;
    admitted.expires_ms = now_ms + kPendingAdmitTtlMs;

    if (credential.kind == ConnectCredential::Kind::kMigration) {
        auto cached = redeemed_tickets_.find(credential.token);
        if (cached != redeemed_tickets_.end()) {
            admitted.ticket = cached->second;
            admitted.has_ticket = true;
        } else if (coordinator_.accept_handoff(credential.token, &admitted.ticket)) {
            admitted.has_ticket = true;
            redeemed_tickets_[credential.token] = admitted.ticket;
        }
        if (!admitted.has_ticket || admitted.ticket.player_id != credential.player_id) {
            ++stats_.denied_connects;
            return decision;
        }
        admitted.region = admitted.ticket.region;
    } else if (config_.advertise) {
        PresenceRecord record;
        if (!coordinator_.registry().get_presence(credential.player_id, &record) ||
            record.session_token != credential.token) {
            ++stats_.denied_connects;
            return decision;
        }
        if (record.shard_id != config_.shard_id) {
            decision.reason = DenyReason::kWrongRegion;
            ++stats_.denied_connects;
            return decision;
        }
        admitted.region = record.region;
    }

    if (!coordinator_.owns_region(admitted.region) && config_.advertise) {
        std::vector<u32> owned = coordinator_.owned_regions();
        if (!owned.empty()) admitted.region = owned.front();
    }

    admitted.client = next_client_++;
    pending_admits_[admitted.client] = admitted;
    pending_by_player_[credential.player_id] = admitted.client;

    decision.admit = true;
    decision.client_id = admitted.client;
    MORTON_LOG_DEBUG("shard %s admitting %s from %s", config_.shard_id.c_str(),
                     credential.player_id.c_str(), from.to_string().c_str());
    return decision;
}

void WorldServer::on_connect(Connection& connection) {
    auto pending = pending_admits_.find(connection.client_id);
    if (pending == pending_admits_.end()) {
        connections_.disconnect(connection, "no admit record");
        return;
    }
    PendingAdmit admitted = pending->second;
    pending_admits_.erase(pending);
    pending_by_player_.erase(admitted.credential.player_id);

    auto existing = by_player_id_.find(admitted.credential.player_id);
    if (existing != by_player_id_.end() && existing->second != connection.client_id) {
        Connection* stale = connections_.find(existing->second);
        if (stale != nullptr) connections_.disconnect(*stale, "replaced by a newer session");
    }

    WorldPlayer player;
    player.client = connection.client_id;
    player.player_id = admitted.credential.player_id;
    player.region = admitted.region;
    player.session_token = admitted.credential.token;
    player.replication.configure(config_.interest, config_.params);

    Vec2 position;
    Vec2 velocity;
    u32 last_sequence = 0;

    if (admitted.has_ticket) {
        position = admitted.ticket.position;
        velocity = admitted.ticket.velocity;
        last_sequence = admitted.ticket.last_input_sequence;
        player.migrated_in = true;
        ++stats_.migrations_in;
        if (migrations_in_counter_) migrations_in_counter_->add();
    } else {
        auto ghost = ghosts_.find(player.player_id);
        if (ghost != ghosts_.end() && ghost->second.expires_ms > wall_ms() &&
            ghost->second.session_token == player.session_token) {
            position = ghost->second.position;
            velocity = ghost->second.velocity;
            last_sequence = ghost->second.last_input_sequence;
            player.region = ghost->second.region;
            ++stats_.reconnects_resumed;
            ghosts_.erase(ghost);
        } else {
            f32 cx = 0.f;
            f32 cy = 0.f;
            config_.regions.center_of(player.region, &cx, &cy);
            u32 spread = hash_u32(static_cast<u32>(connection.client_id) * 2654435761u);
            f32 offset = config_.regions.region_size() * 0.25f;
            position.x = cx + (static_cast<f32>(spread % 2000) / 1000.f - 1.f) * offset;
            position.y = cy + (static_cast<f32>((spread >> 11) % 2000) / 1000.f - 1.f) * offset;
        }
    }

    player.entity = world_.spawn_player(connection.client_id, position);
    world_.adopt_entity(player.entity, connection.client_id, position, velocity, last_sequence);
    player.last_input_sequence = last_sequence;
    connection.entity = player.entity;

    by_player_id_[player.player_id] = connection.client_id;
    players_.emplace(connection.client_id, std::move(player));

    if (config_.advertise) {
        PresenceRecord record;
        record.player_id = admitted.credential.player_id;
        record.shard_id = config_.shard_id;
        record.region = players_[connection.client_id].region;
        record.session_token = admitted.credential.token;
        coordinator_.touch_presence(record);
    }

    WorldConfigEvent event;
    event.entity = players_[connection.client_id].entity;
    event.tick = world_.tick();
    event.region = players_[connection.client_id].region;
    event.world_size = config_.params.size;
    event.tick_rate = config_.params.tick_rate;
    event.shard_id = config_.shard_id;

    u8 buffer[kMaxReliableMessageSize];
    u32 size = encode_world_config(event, buffer, sizeof(buffer));
    if (size > 0) send_event(connection, buffer, size);

    MORTON_LOG_INFO("shard %s: %s joined as entity %u in region %u", config_.shard_id.c_str(),
                    admitted.credential.player_id.c_str(), event.entity, event.region);
}

void WorldServer::on_disconnect(Connection& connection, const char* reason) {
    auto it = players_.find(connection.client_id);
    if (it == players_.end()) return;

    WorldPlayer& player = it->second;

    if (!player.redirecting) {
        i64 index = world_.entities().find(player.entity);
        if (index >= 0) {
            GhostState ghost;
            ghost.position = world_.entities().position[static_cast<std::size_t>(index)];
            ghost.velocity = world_.entities().velocity[static_cast<std::size_t>(index)];
            ghost.last_input_sequence =
                world_.entities().last_input_sequence[static_cast<std::size_t>(index)];
            ghost.region = player.region;
            ghost.session_token = player.session_token;
            ghost.expires_ms = wall_ms() + config_.reconnect_grace_ms;
            ghosts_[player.player_id] = ghost;
        }
        ++stats_.timeouts;
    }

    world_.despawn(player.entity);
    auto mapping = by_player_id_.find(player.player_id);
    if (mapping != by_player_id_.end() && mapping->second == connection.client_id) {
        by_player_id_.erase(mapping);
    }
    MORTON_LOG_INFO("shard %s: %s left (%s)", config_.shard_id.c_str(), player.player_id.c_str(),
                    reason);
    players_.erase(it);
}

void WorldServer::on_payload(Connection& connection, const u8* data, u32 size) {
    if (size == 0) return;
    if (data[0] != static_cast<u8>(Channel::kInput)) return;

    WorldPlayer* player = player_of(connection.client_id);
    if (player == nullptr || player->redirecting) return;

    InputPacket packet;
    if (!decode_input_packet(data, size, &packet)) return;

    for (MoveInput& input : packet.inputs) {
        quantize_input(&input);
        world_.queue_input(connection.client_id, input);
        if (input.sequence > player->last_input_sequence) {
            player->last_input_sequence = input.sequence;
        }
    }
    stats_.inputs_received += packet.inputs.size();
    if (inputs_counter_) inputs_counter_->add(packet.inputs.size());

    if (packet.acked_snapshot_tick != 0 &&
        packet.acked_snapshot_tick != player->acked_snapshot_tick) {
        player->acked_snapshot_tick = packet.acked_snapshot_tick;
        player->replication.acknowledge(packet.acked_snapshot_tick);
    }
}

WorldPlayer* WorldServer::player_of(ClientId client) {
    auto it = players_.find(client);
    return it == players_.end() ? nullptr : &it->second;
}

const WorldPlayer* WorldServer::player_by_id(const std::string& player_id) const {
    auto mapping = by_player_id_.find(player_id);
    if (mapping == by_player_id_.end()) return nullptr;
    auto it = players_.find(mapping->second);
    return it == players_.end() ? nullptr : &it->second;
}

void WorldServer::send_event(Connection& connection, const u8* data, u32 size) {
    connection.reliability.queue_reliable(static_cast<u8>(Channel::kEvent), data, size);
}

void WorldServer::replicate(u64 now_us) {
    u32 capacity = std::min<u32>(config_.interest.max_snapshot_bytes,
                                 kMaxDatagramSize - 64 - kSnapshotChannelBytes);

    for (auto& [client, player] : players_) {
        if (player.redirecting) continue;
        Connection* connection = connections_.find(client);
        if (connection == nullptr) continue;

        scratch_[0] = static_cast<u8>(Channel::kSnapshot);
        u32 size = player.replication.encode(world_, player.entity, player.last_input_sequence,
                                             scratch_.data() + kSnapshotChannelBytes, capacity);
        if (size == 0) continue;

        connections_.send_payload(*connection, scratch_.data(), size + kSnapshotChannelBytes,
                                  now_us);
        ++stats_.snapshots_sent;
        stats_.snapshot_bytes += size;
        if (snapshots_counter_) snapshots_counter_->add();
        if (snapshot_bytes_counter_) snapshot_bytes_counter_->add(size);
        if (snapshot_histogram_) snapshot_histogram_->record(static_cast<f64>(size));
    }
}

void WorldServer::check_migrations() {
    if (!config_.advertise) return;

    std::vector<ClientId> expired;

    for (auto& [client, player] : players_) {
        if (player.redirecting) {
            if (world_.tick() >= player.redirect_deadline) expired.push_back(client);
            continue;
        }

        i64 index = world_.entities().find(player.entity);
        if (index < 0) continue;
        Vec2 position = world_.entities().position[static_cast<std::size_t>(index)];
        Vec2 velocity = world_.entities().velocity[static_cast<std::size_t>(index)];

        u32 target = config_.regions.handoff_target(player.region, position.x, position.y);
        if (target == kInvalidRegion) continue;

        if (coordinator_.owns_region(target)) {
            player.region = target;
            continue;
        }

        HandoffPlan plan;
        if (!coordinator_.plan_handoff(player.player_id, player.region, position, velocity,
                                       world_.tick(), player.last_input_sequence, &plan)) {
            continue;
        }

        MigrateRedirectEvent event;
        event.ticket = plan.token;
        event.region = plan.target_region;
        event.endpoint = plan.target_endpoint;
        event.shard_id = plan.target_shard;

        u8 buffer[kMaxReliableMessageSize];
        u32 size = encode_migrate_redirect(event, buffer, sizeof(buffer));
        Connection* connection = connections_.find(client);
        if (size == 0 || connection == nullptr) continue;
        send_event(*connection, buffer, size);

        player.redirecting = true;
        player.redirect_deadline = world_.tick() + config_.redirect_grace_ticks;
        ++stats_.migrations_out;
        if (migrations_out_counter_) migrations_out_counter_->add();

        MORTON_LOG_INFO("shard %s: handing %s to %s (region %u)", config_.shard_id.c_str(),
                        player.player_id.c_str(), plan.target_shard.c_str(), plan.target_region);
    }

    for (ClientId client : expired) {
        Connection* connection = connections_.find(client);
        if (connection != nullptr) {
            connections_.disconnect(*connection, "migrated to another shard");
        } else {
            auto it = players_.find(client);
            if (it != players_.end()) {
                world_.despawn(it->second.entity);
                by_player_id_.erase(it->second.player_id);
                players_.erase(it);
            }
        }
    }
}

void WorldServer::refresh_cluster(u64 now_us) {
    if (!config_.advertise) return;

    u64 now_ms = wall_ms();
    coordinator_.refresh(now_ms, static_cast<u32>(players_.size()), tick_p99_ms());

    if (now_ms - last_presence_refresh_ms_ >= kPresenceRefreshMs) {
        last_presence_refresh_ms_ = now_ms;
        for (const auto& [client, player] : players_) {
            if (player.redirecting) continue;
            PresenceRecord record;
            record.player_id = player.player_id;
            record.shard_id = config_.shard_id;
            record.region = player.region;
            record.session_token = player.session_token;
            coordinator_.touch_presence(record);
        }
        expire_ghosts(now_ms);

        for (auto it = redeemed_tickets_.begin(); it != redeemed_tickets_.end();) {
            it = (it->second.issued_ms + kRedeemedTicketTtlMs < now_ms) ? redeemed_tickets_.erase(it)
                                                                       : std::next(it);
        }
    }
    (void)now_us;
}

void WorldServer::expire_ghosts(u64 now_ms) {
    for (auto it = ghosts_.begin(); it != ghosts_.end();) {
        it = (it->second.expires_ms <= now_ms) ? ghosts_.erase(it) : std::next(it);
    }
}

void WorldServer::tick(u64 now) {
    u64 started = now_us();

    connections_.receive(now);
    world_.step();
    replicate(now);
    check_migrations();
    connections_.timeout_connections(now);
    connections_.flush(now);
    refresh_cluster(now);

    ++stats_.ticks;
    f64 elapsed = static_cast<f64>(now_us() - started) / 1000000.0;
    if (tick_histogram_) tick_histogram_->record(elapsed);
    if (players_gauge_) players_gauge_->set(static_cast<f64>(players_.size()));
    if (entities_gauge_) entities_gauge_->set(static_cast<f64>(world_.entities().size()));
}

void WorldServer::run() {
    const u64 tick_us = 1000000ull / config_.params.tick_rate;
    u64 next = now_us();

    while (running_.load(std::memory_order_relaxed)) {
        precise_sleep_until(next);
        u64 current = now_us();
        tick(current);
        next += tick_us;

        u64 after = now_us();
        if (after > next + tick_us * 4) next = after;
    }
}

f64 WorldServer::tick_p99_ms() const {
    return tick_histogram_ == nullptr ? 0.0 : tick_histogram_->p99() * 1000.0;
}

std::string WorldServer::state_json() const {
    std::string out = "{\"shard\":\"" + config_.shard_id + "\",\"tick\":" +
                      std::to_string(world_.tick()) + ",\"world_size\":" +
                      number(config_.params.size, 1) + ",\"regions_per_axis\":" +
                      std::to_string(config_.regions.regions_per_axis) + ",\"entities\":[";

    const EntityStore& entities = world_.entities();
    for (u32 i = 0; i < entities.size(); ++i) {
        if (i > 0) out += ",";
        out += "{\"id\":" + std::to_string(entities.id[i]) + ",\"x\":" +
               number(entities.position[i].x, 2) + ",\"y\":" + number(entities.position[i].y, 2) +
               ",\"kind\":" + std::to_string(entities.kind[i]) + "}";
    }
    out += "],\"players\":" + std::to_string(players_.size()) +
           ",\"tick_p99_ms\":" + number(tick_p99_ms()) + "}";
    return out;
}

}  // namespace morton
