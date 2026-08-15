#pragma once
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/types.h"
#include "net/address.h"
#include "net/protocol.h"
#include "net/reliability.h"
#include "net/udp_socket.h"

namespace morton {

using ConnectToken = std::array<u8, kConnectTokenBytes>;

/// One authenticated peer. Owned by ConnectionServer, keyed by source address.
struct Connection {
    ClientId client_id = 0;
    Address address;
    u64 salt = 0;
    ConnectToken token{};
    EntityId entity = kInvalidEntity;
    u64 last_receive_us = 0;
    u64 last_send_us = 0;
    u64 connected_at_us = 0;
    bool disconnecting = false;
    ReliableEndpoint reliability;
};

struct ServerConfig {
    Address bind;
    u32 max_connections = 4096;
    u64 timeout_us = 5000000;
    u64 keepalive_us = 100000;
};

/// Result of validating a connect token, supplied by the owning service.
struct AdmitDecision {
    bool admit = false;
    DenyReason reason = DenyReason::kBadToken;
    ClientId client_id = 0;
};

/// UDP server managing many client connections: handshake, timeouts, keepalives,
/// and reliable delivery. Transport only; it knows nothing about simulation.
class ConnectionServer {
public:
    using AdmitFn = std::function<AdmitDecision(const Address&, const ConnectToken&)>;
    using ConnectFn = std::function<void(Connection&)>;
    using DisconnectFn = std::function<void(Connection&, const char* reason)>;
    using PayloadFn = std::function<void(Connection&, const u8* data, u32 size)>;
    using MessageFn = std::function<void(Connection&, const ReliableMessage&)>;

    bool start(const ServerConfig& config);
    void stop();

    /// Drains the socket, running handshakes and dispatching payloads. Returns
    /// the number of datagrams processed.
    u32 receive(u64 now_us);

    /// Sends queued reliable messages plus keepalives to any idle connection.
    void flush(u64 now_us);

    /// Sends one payload packet to a connection, carrying `data` unreliably.
    bool send_payload(Connection& connection, const u8* data, u32 size, u64 now_us);

    /// Drops connections that have gone silent past the configured timeout.
    void timeout_connections(u64 now_us);

    void disconnect(Connection& connection, const char* reason);

    void set_admit_handler(AdmitFn fn) { on_admit_ = std::move(fn); }
    void set_connect_handler(ConnectFn fn) { on_connect_ = std::move(fn); }
    void set_disconnect_handler(DisconnectFn fn) { on_disconnect_ = std::move(fn); }
    void set_payload_handler(PayloadFn fn) { on_payload_ = std::move(fn); }
    void set_message_handler(MessageFn fn) { on_message_ = std::move(fn); }

    Connection* find(ClientId client_id);
    u32 connection_count() const { return static_cast<u32>(by_client_.size()); }
    Address local_address() const { return socket_.local_address(); }
    UdpSocket& socket() { return socket_; }

    template <typename Fn>
    void for_each_connection(Fn&& fn) {
        for (auto& [address, connection] : connections_) fn(*connection);
    }

    u64 handshakes_completed() const { return handshakes_completed_; }
    u64 handshakes_denied() const { return handshakes_denied_; }
    u64 invalid_packets() const { return invalid_packets_; }

private:
    void handle_connection_request(const Address& from, const u8* data, u32 size, u64 now_us);
    void handle_challenge_response(const Address& from, const u8* data, u32 size, u64 now_us);
    void handle_payload(const Address& from, const u8* data, u32 size, u64 now_us);
    void send_challenge(const Address& to, u64 client_salt, u64 server_salt);
    void send_denied(const Address& to, DenyReason reason);
    void send_accepted(Connection& connection);
    void remove_connection(const Address& address, const char* reason);

    struct PendingHandshake {
        u64 client_salt = 0;
        u64 server_salt = 0;
        ConnectToken token{};
        u64 started_us = 0;
    };

    ServerConfig config_;
    UdpSocket socket_;
    std::unordered_map<Address, std::unique_ptr<Connection>, AddressHash> connections_;
    std::unordered_map<Address, PendingHandshake, AddressHash> pending_;
    std::unordered_map<ClientId, Connection*> by_client_;

    AdmitFn on_admit_;
    ConnectFn on_connect_;
    DisconnectFn on_disconnect_;
    PayloadFn on_payload_;
    MessageFn on_message_;

    std::vector<ReliableMessage> message_scratch_;
    u64 handshakes_completed_ = 0;
    u64 handshakes_denied_ = 0;
    u64 invalid_packets_ = 0;
};

enum class ClientState : u8 {
    kDisconnected,
    kSendingRequest,
    kSendingResponse,
    kConnected,
    kDenied,
    kTimedOut,
};

struct ClientConfig {
    Address server;
    ConnectToken token{};
    u64 timeout_us = 5000000;
    u64 retry_interval_us = 100000;
    u64 keepalive_us = 100000;
    Address bind = Address(INADDR_ANY, 0);
};

/// Client half of the same protocol, driven by explicit update calls so it can be
/// stepped by a game loop or by thousands of instances inside a load generator.
class ClientConnection {
public:
    using PayloadFn = std::function<void(const u8* data, u32 size)>;
    using MessageFn = std::function<void(const ReliableMessage&)>;

    bool start(const ClientConfig& config);
    void stop();

    /// Drives the handshake, drains the socket and dispatches payloads.
    void update(u64 now_us);

    bool send_payload(const u8* data, u32 size, u64 now_us);
    void queue_reliable(u8 channel, const u8* data, u32 size);
    void disconnect();

    ClientState state() const { return state_; }
    bool connected() const { return state_ == ClientState::kConnected; }
    DenyReason deny_reason() const { return deny_reason_; }
    ClientId client_id() const { return client_id_; }
    EntityId entity() const { return entity_; }
    Tick accepted_tick() const { return accepted_tick_; }

    /// Repoints this client at a different shard, keeping the socket open.
    void redirect(const Address& server, const ConnectToken& token, u64 now_us);

    const NetworkStats& stats() const { return reliability_.stats(); }
    ReliableEndpoint& reliability() { return reliability_; }
    UdpSocket& socket() { return socket_; }

    void set_payload_handler(PayloadFn fn) { on_payload_ = std::move(fn); }
    void set_message_handler(MessageFn fn) { on_message_ = std::move(fn); }

private:
    void send_connection_request(u64 now_us);
    void send_challenge_response(u64 now_us);
    void process_datagram(const u8* data, u32 size, u64 now_us);

    ClientConfig config_;
    UdpSocket socket_;
    ClientState state_ = ClientState::kDisconnected;
    DenyReason deny_reason_ = DenyReason::kBadToken;

    u64 client_salt_ = 0;
    u64 server_salt_ = 0;
    u64 last_send_us_ = 0;
    u64 last_receive_us_ = 0;
    u64 connect_started_us_ = 0;

    ClientId client_id_ = 0;
    EntityId entity_ = kInvalidEntity;
    Tick accepted_tick_ = 0;

    ReliableEndpoint reliability_;
    PayloadFn on_payload_;
    MessageFn on_message_;
    std::vector<ReliableMessage> message_scratch_;
};

/// Cryptographically seeded 64-bit value used for handshake salts.
u64 generate_salt();

}  // namespace morton
