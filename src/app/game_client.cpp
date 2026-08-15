#include "app/game_client.h"

#include <cstdlib>

#include "core/log.h"
#include "core/time.h"
#include "net/udp_socket.h"

namespace morton {

bool GameClient::build_credential(ConnectCredential::Kind kind, u64 token,
                                  ConnectToken* out) const {
    ConnectCredential credential;
    credential.kind = kind;
    credential.token = token;
    credential.player_id = config_.player_id;
    return encode_credential(credential, out);
}

bool GameClient::request_session() {
    if (!config_.matchmaker.valid()) return false;

    HttpFetch response = http_fetch(config_.matchmaker, "POST",
                                    "/session?player=" + config_.player_id);
    if (!response.ok) {
        MORTON_LOG_WARN("matchmaker rejected %s: %s", config_.player_id.c_str(),
                        response.error.empty() ? response.body.c_str() : response.error.c_str());
        return false;
    }

    std::string endpoint = json_lookup(response.body, "endpoint");
    std::string token = json_lookup(response.body, "token");
    shard_id_ = json_lookup(response.body, "shard");
    region_ = static_cast<u32>(std::strtoul(json_lookup(response.body, "region").c_str(), nullptr, 10));

    if (endpoint.empty() || token.empty()) return false;
    if (!Address::parse(endpoint, &shard_endpoint_)) return false;

    session_token_ = std::strtoull(token.c_str(), nullptr, 10);
    if (json_lookup(response.body, "reconnect") == "true") ++stats_.reconnects;
    return session_token_ != 0;
}

bool GameClient::start(const GameClientConfig& config) {
    config_ = config;
    view_.configure(config_.params, config_.view);

    if (config_.direct_shard.valid()) {
        shard_endpoint_ = config_.direct_shard;
        session_token_ = config_.session_token;
    } else if (!request_session()) {
        return false;
    }

    ClientConfig client;
    client.server = shard_endpoint_;
    client.timeout_us = config_.timeout_us;
    client.bind = config_.bind;
    if (!build_credential(ConnectCredential::Kind::kSession, session_token_, &client.token)) {
        return false;
    }

    connection_.set_payload_handler([this](const u8* data, u32 size) { handle_payload(data, size); });
    connection_.set_message_handler(
        [this](const ReliableMessage& message) { handle_message(message); });

    return connection_.start(client);
}

void GameClient::stop() { connection_.stop(); }

void GameClient::handle_payload(const u8* data, u32 size) {
    if (size <= 1 || data[0] != static_cast<u8>(Channel::kSnapshot)) return;
    ++stats_.snapshots_received;
    stats_.bytes_received += size;
    if (view_.apply_snapshot(data + 1, size - 1)) ++stats_.snapshots_applied;
}

void GameClient::handle_message(const ReliableMessage& message) {
    if (message.channel != static_cast<u8>(Channel::kEvent) || message.payload.empty()) return;

    switch (static_cast<EventType>(message.payload[0])) {
        case EventType::kWorldConfig: {
            WorldConfigEvent event;
            if (!decode_world_config(message.payload.data(), static_cast<u32>(message.payload.size()), &event)) return;
            local_entity_ = event.entity;
            region_ = event.region;
            shard_id_ = event.shard_id;
            view_.set_local_entity(event.entity);
            break;
        }
        case EventType::kMigrateRedirect: {
            MigrateRedirectEvent event;
            if (!decode_migrate_redirect(message.payload.data(), static_cast<u32>(message.payload.size()), &event)) return;
            redirect_ = event;
            pending_redirect_ = true;
            break;
        }
        default:
            break;
    }
}

void GameClient::apply_redirect(u64 now_us) {
    pending_redirect_ = false;

    Address endpoint;
    if (!Address::parse(redirect_.endpoint, &endpoint)) return;

    ConnectToken token;
    if (!build_credential(ConnectCredential::Kind::kMigration, redirect_.ticket, &token)) return;

    Vec2 position = view_.predicted_position();
    Vec2 velocity = Vec2{};
    for (const RenderEntity& entity : view_.render_entities()) {
        if (entity.is_local) velocity = entity.velocity;
    }

    connection_.redirect(endpoint, token, now_us);
    shard_endpoint_ = endpoint;
    shard_id_ = redirect_.shard_id;
    region_ = redirect_.region;
    local_entity_ = kInvalidEntity;
    ++stats_.migrations;

    view_.adopt_migrated_state(kInvalidEntity, position, velocity);
    MORTON_LOG_DEBUG("client %s redirected to %s", config_.player_id.c_str(),
                     redirect_.endpoint.c_str());
}

void GameClient::update(u64 now) {
    connection_.update(now);
    if (pending_redirect_) apply_redirect(now);

    if (last_update_us_ != 0 && now > last_update_us_) {
        view_.advance(static_cast<f32>(now - last_update_us_) / 1000000.f);
    }
    last_update_us_ = now;
}

void GameClient::send_input(const Vec2& axis, bool sprint, u64 now_us) {
    if (!connection_.connected() || local_entity_ == kInvalidEntity) return;

    ++client_tick_;
    MoveInput input = view_.push_input(axis, sprint, client_tick_);

    input_window_.push_back(input);
    if (input_window_.size() > kInputRedundancy) {
        input_window_.erase(input_window_.begin());
    }

    InputPacket packet;
    packet.client_tick = client_tick_;
    packet.acked_snapshot_tick = view_.latest_snapshot_tick();
    packet.inputs = input_window_;

    u8 buffer[kMaxDatagramSize];
    u32 size = encode_input_packet(packet, buffer, sizeof(buffer));
    if (size == 0) return;

    if (connection_.send_payload(buffer, size, now_us)) {
        ++stats_.inputs_sent;
        stats_.bytes_sent += size;
    }
}

}  // namespace morton
