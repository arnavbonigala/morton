#pragma once
#include <string>
#include <vector>

#include "net/connection.h"
#include "net/http_client.h"
#include "proto/messages.h"
#include "sim/client_view.h"

namespace morton {

struct GameClientConfig {
    std::string player_id;
    Address matchmaker;
    Address direct_shard;
    u64 session_token = 0;
    WorldParams params;
    ClientViewConfig view;
    u64 timeout_us = 5000000;
    Address bind = Address(INADDR_ANY, 0);
};

struct GameClientStats {
    u32 snapshots_received = 0;
    u32 snapshots_applied = 0;
    u32 migrations = 0;
    u32 reconnects = 0;
    u32 inputs_sent = 0;
    u64 bytes_received = 0;
    u64 bytes_sent = 0;
};

/// A full client: matchmaking, handshake, prediction, reconciliation,
/// interpolation and shard migration.
///
/// Migration is invisible to the caller. On a redirect the client keeps its
/// predicted state and repoints the same socket at the new shard, so the avatar
/// does not rubber-band to the origin while the first snapshot is in flight.
class GameClient {
public:
    bool start(const GameClientConfig& config);
    void stop();

    /// Asks the matchmaker for a shard and session token. Not required when
    /// `direct_shard` is configured.
    bool request_session();

    void update(u64 now_us);

    /// Queues a command for this tick and sends the recent command window.
    void send_input(const Vec2& axis, bool sprint, u64 now_us);

    bool connected() const { return connection_.connected(); }
    ClientState state() const { return connection_.state(); }
    DenyReason deny_reason() const { return connection_.deny_reason(); }
    const std::string& shard_id() const { return shard_id_; }
    Address shard_endpoint() const { return shard_endpoint_; }
    bool migrating() const { return pending_redirect_; }

    ClientView& view() { return view_; }
    const ClientView& view() const { return view_; }
    ClientConnection& connection() { return connection_; }
    const NetworkStats& net_stats() const { return connection_.stats(); }
    const GameClientStats& stats() const { return stats_; }
    EntityId local_entity() const { return local_entity_; }
    u32 region() const { return region_; }

private:
    void handle_payload(const u8* data, u32 size);
    void handle_message(const ReliableMessage& message);
    void apply_redirect(u64 now_us);
    bool build_credential(ConnectCredential::Kind kind, u64 token, ConnectToken* out) const;

    GameClientConfig config_;
    ClientConnection connection_;
    ClientView view_;

    std::string shard_id_;
    Address shard_endpoint_;
    u64 session_token_ = 0;
    EntityId local_entity_ = kInvalidEntity;
    u32 region_ = 0;

    bool pending_redirect_ = false;
    MigrateRedirectEvent redirect_;

    std::vector<MoveInput> input_window_;
    Tick client_tick_ = 0;
    u64 last_update_us_ = 0;
    GameClientStats stats_;
};

}  // namespace morton
