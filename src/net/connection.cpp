#include "net/connection.h"

#include <atomic>
#include <random>

#include "core/log.h"
#include "core/time.h"
#include "proto/bitstream.h"

namespace morton {
namespace {

constexpr u32 kHeaderBytes = 5;
constexpr u64 kHandshakeTimeoutUs = 5000000;

/// The connection request is padded to the largest packet the protocol uses so a
/// spoofed request can never elicit a larger reply than it cost to send.
constexpr u32 kConnectionRequestPadding = 1000;

ClientId next_client_id() {
    static std::atomic<ClientId> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

u64 generate_salt() {
    static thread_local std::random_device device;
    static thread_local std::mt19937_64 rng(
        (static_cast<u64>(device()) << 32) ^ device() ^ now_us());
    u64 salt = rng();
    return salt == 0 ? 1 : salt;
}

namespace {

u32 write_header(u8* buffer, u32 capacity, PacketType type) {
    BitWriter writer(buffer, capacity);
    writer.write_u32(kProtocolId);
    writer.write_u8(static_cast<u8>(type));
    return writer.bytes_written();
}

bool read_header(const u8* data, u32 size, PacketType* out_type) {
    if (size < kHeaderBytes) return false;
    BitReader reader(data, size);
    if (reader.read_u32() != kProtocolId) return false;
    u8 type = reader.read_u8();
    if (type < 1 || type > 7) return false;
    *out_type = static_cast<PacketType>(type);
    return true;
}

}  // namespace

bool ConnectionServer::start(const ServerConfig& config) {
    config_ = config;
    if (!socket_.open(config.bind)) return false;
    MORTON_LOG_INFO("connection server listening on %s",
                    socket_.local_address().to_string().c_str());
    return true;
}

void ConnectionServer::stop() {
    for (auto& [address, connection] : connections_) {
        u8 buffer[64];
        u32 offset = write_header(buffer, sizeof(buffer), PacketType::kDisconnect);
        BitWriter writer(buffer + offset, sizeof(buffer) - offset);
        writer.write_u64(connection->salt);
        socket_.send(address, buffer, offset + writer.bytes_written());
    }
    connections_.clear();
    by_client_.clear();
    pending_.clear();
    socket_.close();
}

u32 ConnectionServer::receive(u64 now_us) {
    u8 buffer[kMaxDatagramSize];
    Address from;
    u32 processed = 0;

    while (true) {
        int size = socket_.receive(&from, buffer, sizeof(buffer));
        if (size <= 0) break;
        ++processed;

        PacketType type;
        if (!read_header(buffer, static_cast<u32>(size), &type)) {
            ++invalid_packets_;
            continue;
        }

        const u8* body = buffer + kHeaderBytes;
        u32 body_size = static_cast<u32>(size) - kHeaderBytes;

        switch (type) {
            case PacketType::kConnectionRequest:
                handle_connection_request(from, body, body_size, now_us);
                break;
            case PacketType::kChallengeResponse:
                handle_challenge_response(from, body, body_size, now_us);
                break;
            case PacketType::kPayload:
                handle_payload(from, body, body_size, now_us);
                break;
            case PacketType::kDisconnect: {
                BitReader reader(body, body_size);
                u64 salt = reader.read_u64();
                auto it = connections_.find(from);
                if (it != connections_.end() && it->second->salt == salt) {
                    remove_connection(from, "peer disconnected");
                }
                break;
            }
            default:
                ++invalid_packets_;
                break;
        }
    }

    for (auto it = pending_.begin(); it != pending_.end();) {
        if (now_us - it->second.started_us > kHandshakeTimeoutUs) {
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }

    return processed;
}

void ConnectionServer::handle_connection_request(const Address& from, const u8* data, u32 size,
                                                 u64 now_us) {
    if (size < 8 + kConnectTokenBytes + kConnectionRequestPadding) {
        ++invalid_packets_;
        return;
    }

    BitReader reader(data, size);
    u64 client_salt = reader.read_u64();
    ConnectToken token;
    reader.read_bytes(token.data(), kConnectTokenBytes);
    if (reader.overflowed() || client_salt == 0) {
        ++invalid_packets_;
        return;
    }

    auto existing = connections_.find(from);
    if (existing != connections_.end()) {
        send_accepted(*existing->second);
        return;
    }

    if (connections_.size() >= config_.max_connections) {
        send_denied(from, DenyReason::kServerFull);
        ++handshakes_denied_;
        return;
    }

    auto& pending = pending_[from];
    if (pending.client_salt != client_salt) {
        pending.client_salt = client_salt;
        pending.server_salt = generate_salt();
        pending.started_us = now_us;
    }
    pending.token = token;

    send_challenge(from, pending.client_salt, pending.server_salt);
}

void ConnectionServer::handle_challenge_response(const Address& from, const u8* data, u32 size,
                                                 u64 now_us) {
    BitReader reader(data, size);
    u64 xor_salt = reader.read_u64();
    if (reader.overflowed()) {
        ++invalid_packets_;
        return;
    }

    auto existing = connections_.find(from);
    if (existing != connections_.end()) {
        if (existing->second->salt == xor_salt) send_accepted(*existing->second);
        return;
    }

    auto it = pending_.find(from);
    if (it == pending_.end()) {
        ++invalid_packets_;
        return;
    }

    const PendingHandshake& pending = it->second;
    if ((pending.client_salt ^ pending.server_salt) != xor_salt) {
        ++invalid_packets_;
        return;
    }

    AdmitDecision decision;
    decision.admit = true;
    decision.client_id = next_client_id();
    if (on_admit_) decision = on_admit_(from, pending.token);

    if (!decision.admit) {
        send_denied(from, decision.reason);
        pending_.erase(it);
        ++handshakes_denied_;
        return;
    }

    auto connection = std::make_unique<Connection>();
    connection->client_id = decision.client_id != 0 ? decision.client_id : next_client_id();
    connection->address = from;
    connection->salt = xor_salt;
    connection->token = pending.token;
    connection->last_receive_us = now_us;
    connection->last_send_us = now_us;
    connection->connected_at_us = now_us;
    pending_.erase(it);

    Connection* raw = connection.get();
    connections_.emplace(from, std::move(connection));
    by_client_[raw->client_id] = raw;
    ++handshakes_completed_;

    if (on_connect_) on_connect_(*raw);
    send_accepted(*raw);

    MORTON_LOG_DEBUG("client %u connected from %s", raw->client_id,
                     from.to_string().c_str());
}

void ConnectionServer::handle_payload(const Address& from, const u8* data, u32 size, u64 now_us) {
    auto it = connections_.find(from);
    if (it == connections_.end()) {
        ++invalid_packets_;
        return;
    }

    Connection& connection = *it->second;
    BitReader salt_reader(data, size);
    u64 salt = salt_reader.read_u64();
    if (salt_reader.overflowed() || salt != connection.salt) {
        ++invalid_packets_;
        return;
    }

    PayloadView payload;
    message_scratch_.clear();
    if (!connection.reliability.read_packet(data + 8, size - 8, now_us, &payload,
                                            &message_scratch_)) {
        return;
    }

    connection.last_receive_us = now_us;

    if (payload.size > 0 && on_payload_) {
        on_payload_(connection, payload.data, payload.size);
    }
    if (on_message_) {
        for (const ReliableMessage& message : message_scratch_) on_message_(connection, message);
    }
}

void ConnectionServer::send_challenge(const Address& to, u64 client_salt, u64 server_salt) {
    u8 buffer[64];
    u32 offset = write_header(buffer, sizeof(buffer), PacketType::kChallenge);
    BitWriter writer(buffer + offset, sizeof(buffer) - offset);
    writer.write_u64(client_salt);
    writer.write_u64(server_salt);
    socket_.send(to, buffer, offset + writer.bytes_written());
}

void ConnectionServer::send_denied(const Address& to, DenyReason reason) {
    u8 buffer[32];
    u32 offset = write_header(buffer, sizeof(buffer), PacketType::kConnectionDenied);
    BitWriter writer(buffer + offset, sizeof(buffer) - offset);
    writer.write_u8(static_cast<u8>(reason));
    socket_.send(to, buffer, offset + writer.bytes_written());
}

void ConnectionServer::send_accepted(Connection& connection) {
    u8 buffer[64];
    u32 offset = write_header(buffer, sizeof(buffer), PacketType::kConnectionAccepted);
    BitWriter writer(buffer + offset, sizeof(buffer) - offset);
    writer.write_u64(connection.salt);
    writer.write_u32(connection.client_id);
    writer.write_u32(connection.entity);
    socket_.send(connection.address, buffer, offset + writer.bytes_written());
}

bool ConnectionServer::send_payload(Connection& connection, const u8* data, u32 size,
                                    u64 now_us) {
    u8 buffer[kMaxDatagramSize];
    u32 offset = write_header(buffer, sizeof(buffer), PacketType::kPayload);
    BitWriter salt_writer(buffer + offset, sizeof(buffer) - offset);
    salt_writer.write_u64(connection.salt);
    offset += salt_writer.bytes_written();

    u32 body = connection.reliability.write_packet(buffer + offset, sizeof(buffer) - offset, data,
                                                   size, now_us);
    if (body == 0) return false;

    connection.last_send_us = now_us;
    return socket_.send_batched(connection.address, buffer, offset + body);
}

void ConnectionServer::flush(u64 now_us) {
    for (auto& [address, connection] : connections_) {
        connection->reliability.update(now_us);
        bool needs_keepalive = now_us - connection->last_send_us >= config_.keepalive_us;
        if (needs_keepalive || connection->reliability.has_pending_reliable()) {
            send_payload(*connection, nullptr, 0, now_us);
        }
    }
    socket_.flush_sends();
}

void ConnectionServer::timeout_connections(u64 now_us) {
    std::vector<Address> expired;
    for (auto& [address, connection] : connections_) {
        if (now_us - connection->last_receive_us > config_.timeout_us) expired.push_back(address);
    }
    for (const Address& address : expired) remove_connection(address, "timed out");
}

void ConnectionServer::disconnect(Connection& connection, const char* reason) {
    u8 buffer[32];
    u32 offset = write_header(buffer, sizeof(buffer), PacketType::kDisconnect);
    BitWriter writer(buffer + offset, sizeof(buffer) - offset);
    writer.write_u64(connection.salt);
    for (int i = 0; i < 3; ++i) {
        socket_.send(connection.address, buffer, offset + writer.bytes_written());
    }
    remove_connection(connection.address, reason);
}

void ConnectionServer::remove_connection(const Address& address, const char* reason) {
    auto it = connections_.find(address);
    if (it == connections_.end()) return;

    Connection& connection = *it->second;
    MORTON_LOG_DEBUG("client %u removed: %s", connection.client_id, reason);
    if (on_disconnect_) on_disconnect_(connection, reason);
    by_client_.erase(connection.client_id);
    connections_.erase(it);
}

Connection* ConnectionServer::find(ClientId client_id) {
    auto it = by_client_.find(client_id);
    return it == by_client_.end() ? nullptr : it->second;
}

bool ClientConnection::start(const ClientConfig& config) {
    config_ = config;
    if (!socket_.is_open() && !socket_.open(config.bind, 1024 * 1024)) return false;

    client_salt_ = generate_salt();
    server_salt_ = 0;
    state_ = ClientState::kSendingRequest;
    last_send_us_ = 0;
    last_receive_us_ = now_us();
    connect_started_us_ = last_receive_us_;
    reliability_.reset();
    return true;
}

void ClientConnection::stop() {
    disconnect();
    socket_.close();
    state_ = ClientState::kDisconnected;
}

void ClientConnection::redirect(const Address& server, const ConnectToken& token, u64 now) {
    config_.server = server;
    config_.token = token;
    client_salt_ = generate_salt();
    server_salt_ = 0;
    state_ = ClientState::kSendingRequest;
    last_send_us_ = 0;
    last_receive_us_ = now;
    connect_started_us_ = now;
    reliability_.reset();
}

void ClientConnection::send_connection_request(u64 now) {
    u8 buffer[kMaxDatagramSize];
    u32 offset = write_header(buffer, sizeof(buffer), PacketType::kConnectionRequest);
    BitWriter writer(buffer + offset, sizeof(buffer) - offset);
    writer.write_u64(client_salt_);
    writer.write_bytes(config_.token.data(), kConnectTokenBytes);
    for (u32 i = 0; i < kConnectionRequestPadding; ++i) writer.write_u8(0);
    socket_.send(config_.server, buffer, offset + writer.bytes_written());
    last_send_us_ = now;
}

void ClientConnection::send_challenge_response(u64 now) {
    u8 buffer[64];
    u32 offset = write_header(buffer, sizeof(buffer), PacketType::kChallengeResponse);
    BitWriter writer(buffer + offset, sizeof(buffer) - offset);
    writer.write_u64(client_salt_ ^ server_salt_);
    socket_.send(config_.server, buffer, offset + writer.bytes_written());
    last_send_us_ = now;
}

void ClientConnection::update(u64 now) {
    if (state_ == ClientState::kDisconnected || state_ == ClientState::kDenied ||
        state_ == ClientState::kTimedOut) {
        return;
    }

    u8 buffer[kMaxDatagramSize];
    Address from;
    while (true) {
        int size = socket_.receive(&from, buffer, sizeof(buffer));
        if (size <= 0) break;
        if (from != config_.server) continue;
        process_datagram(buffer, static_cast<u32>(size), now);
    }

    reliability_.update(now);

    switch (state_) {
        case ClientState::kSendingRequest:
            if (now - last_send_us_ >= config_.retry_interval_us) send_connection_request(now);
            break;
        case ClientState::kSendingResponse:
            if (now - last_send_us_ >= config_.retry_interval_us) send_challenge_response(now);
            break;
        case ClientState::kConnected:
            if (now - last_send_us_ >= config_.keepalive_us ||
                reliability_.has_pending_reliable()) {
                send_payload(nullptr, 0, now);
            }
            break;
        default:
            break;
    }

    if (now - last_receive_us_ > config_.timeout_us) {
        state_ = ClientState::kTimedOut;
    }
}

void ClientConnection::process_datagram(const u8* data, u32 size, u64 now) {
    PacketType type;
    if (!read_header(data, size, &type)) return;

    const u8* body = data + kHeaderBytes;
    u32 body_size = size - kHeaderBytes;
    BitReader reader(body, body_size);

    switch (type) {
        case PacketType::kChallenge: {
            u64 echoed_client_salt = reader.read_u64();
            u64 server_salt = reader.read_u64();
            if (reader.overflowed() || echoed_client_salt != client_salt_) return;
            if (state_ != ClientState::kSendingRequest) return;
            server_salt_ = server_salt;
            state_ = ClientState::kSendingResponse;
            last_receive_us_ = now;
            send_challenge_response(now);
            break;
        }
        case PacketType::kConnectionAccepted: {
            u64 salt = reader.read_u64();
            ClientId client_id = reader.read_u32();
            EntityId entity = reader.read_u32();
            if (reader.overflowed() || salt != (client_salt_ ^ server_salt_)) return;
            if (state_ == ClientState::kSendingResponse) {
                state_ = ClientState::kConnected;
                MORTON_LOG_DEBUG("connected as client %u", client_id);
            }
            client_id_ = client_id;
            entity_ = entity;
            last_receive_us_ = now;
            break;
        }
        case PacketType::kConnectionDenied: {
            u8 reason = reader.read_u8();
            if (reader.overflowed()) return;
            deny_reason_ = static_cast<DenyReason>(reason);
            state_ = ClientState::kDenied;
            break;
        }
        case PacketType::kDisconnect: {
            u64 salt = reader.read_u64();
            if (!reader.overflowed() && salt == (client_salt_ ^ server_salt_)) {
                state_ = ClientState::kDisconnected;
            }
            break;
        }
        case PacketType::kPayload: {
            u64 salt = reader.read_u64();
            if (reader.overflowed() || salt != (client_salt_ ^ server_salt_)) return;
            if (state_ != ClientState::kConnected) return;

            PayloadView payload;
            message_scratch_.clear();
            if (!reliability_.read_packet(body + 8, body_size - 8, now, &payload,
                                          &message_scratch_)) {
                return;
            }
            last_receive_us_ = now;
            if (payload.size > 0 && on_payload_) on_payload_(payload.data, payload.size);
            if (on_message_) {
                for (const ReliableMessage& message : message_scratch_) on_message_(message);
            }
            break;
        }
        default:
            break;
    }
}

bool ClientConnection::send_payload(const u8* data, u32 size, u64 now) {
    if (state_ != ClientState::kConnected) return false;

    u8 buffer[kMaxDatagramSize];
    u32 offset = write_header(buffer, sizeof(buffer), PacketType::kPayload);
    BitWriter salt_writer(buffer + offset, sizeof(buffer) - offset);
    salt_writer.write_u64(client_salt_ ^ server_salt_);
    offset += salt_writer.bytes_written();

    u32 body =
        reliability_.write_packet(buffer + offset, sizeof(buffer) - offset, data, size, now);
    if (body == 0) return false;

    last_send_us_ = now;
    return socket_.send(config_.server, buffer, offset + body);
}

void ClientConnection::queue_reliable(u8 channel, const u8* data, u32 size) {
    reliability_.queue_reliable(channel, data, size);
}

void ClientConnection::disconnect() {
    if (state_ != ClientState::kConnected) return;
    u8 buffer[32];
    u32 offset = write_header(buffer, sizeof(buffer), PacketType::kDisconnect);
    BitWriter writer(buffer + offset, sizeof(buffer) - offset);
    writer.write_u64(client_salt_ ^ server_salt_);
    for (int i = 0; i < 3; ++i) {
        socket_.send(config_.server, buffer, offset + writer.bytes_written());
    }
    state_ = ClientState::kDisconnected;
}

}  // namespace morton
