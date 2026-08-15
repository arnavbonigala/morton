#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "cluster/shard.h"
#include "core/types.h"
#include "metrics/registry.h"
#include "net/connection.h"
#include "net/http.h"
#include "net/websocket.h"
#include "proto/messages.h"
#include "proto/snapshot.h"
#include "sim/world.h"

namespace morton {

struct WorldServerConfig {
    std::string shard_id = "world-a";
    Address udp_bind;
    Address http_bind;

    /// Optional visualiser bridge, enabled by a non-zero viewer_hz. The shard
    /// then publishes its world state as JSON text frames and serves the
    /// browser client on GET /.
    Address ws_bind;
    u32 viewer_hz = 0;

    Address redis;
    std::string key_prefix = "morton";
    WorldParams params;
    InterestConfig interest;
    RegionMap regions;
    u32 capacity = 512;
    u32 drifters = 0;
    u32 max_connections = 4096;

    /// How long a dropped player's simulation state is held so a reconnect
    /// resumes in place instead of respawning at the region centre.
    u32 reconnect_grace_ms = 20000;

    /// Ticks the source shard keeps simulating a redirected player, giving the
    /// reliable redirect message time to arrive before the session is torn down.
    u32 redirect_grace_ticks = 45;

    u64 connection_timeout_us = 5000000;
    bool advertise = true;

    /// Endpoints peers should dial, when they differ from the bound ones: a
    /// container binds 0.0.0.0 but must be reached by its service name.
    std::string advertise_udp;
    std::string advertise_http;
};

struct WorldPlayer {
    ClientId client = 0;
    std::string player_id;
    EntityId entity = kInvalidEntity;
    u32 region = 0;
    u64 session_token = 0;
    u32 last_input_sequence = 0;
    Tick acked_snapshot_tick = 0;
    Tick redirect_deadline = 0;
    bool redirecting = false;
    bool migrated_in = false;
    ReplicationState replication;
};

/// Simulation state kept behind after a client drops, so a reconnect within the
/// grace window resumes the same avatar rather than starting a new one.
struct GhostState {
    Vec2 position;
    Vec2 velocity;
    u32 last_input_sequence = 0;
    u32 region = 0;
    u64 session_token = 0;
    u64 expires_ms = 0;
};

struct WorldServerStats {
    u64 ticks = 0;
    u64 snapshots_sent = 0;
    u64 snapshot_bytes = 0;
    u64 inputs_received = 0;
    u64 migrations_out = 0;
    u64 migrations_in = 0;
    u64 reconnects_resumed = 0;
    u64 timeouts = 0;
    u64 denied_connects = 0;
};

/// One authoritative shard: UDP transport, fixed-tick simulation, relevance
/// based replication, cluster membership and cross-shard handoff in one process.
class WorldServer {
public:
    bool start(const WorldServerConfig& config);
    void stop();

    /// Runs one simulation tick end to end. Public so tests and the load
    /// harness can drive the shard deterministically instead of racing a thread.
    void tick(u64 now_us);

    /// Blocking fixed-rate loop until stop() is called from another thread.
    void run();
    void request_stop() { running_.store(false, std::memory_order_relaxed); }
    bool running() const { return running_.load(std::memory_order_relaxed); }

    World& world() { return world_; }
    ShardCoordinator& coordinator() { return coordinator_; }
    ConnectionServer& connections() { return connections_; }
    const WorldServerStats& stats() const { return stats_; }

    const WorldServerConfig& config() const { return config_; }

    Address udp_address() const { return connections_.local_address(); }
    Address http_address() const { return http_.local_address(); }
    Address viewer_address() const { return viewer_.local_address(); }
    u32 viewer_count() const { return viewer_.client_count(); }
    u32 player_count() const { return static_cast<u32>(players_.size()); }

    /// Players this shard still owns, excluding those already handed to another
    /// shard and only held open until their redirect grace expires.
    u32 resident_player_count() const;
    f64 tick_p99_ms() const;

    const WorldPlayer* player_by_id(const std::string& player_id) const;

    std::string state_json() const;

private:
    AdmitDecision admit(const Address& from, const ConnectToken& token);
    void on_connect(Connection& connection);
    void on_disconnect(Connection& connection, const char* reason);
    void on_payload(Connection& connection, const u8* data, u32 size);

    void replicate(u64 now_us);
    void check_migrations();
    void refresh_cluster(u64 now_us);
    void publish_viewer_state(u64 now_us);
    void expire_ghosts(u64 now_ms);
    void send_event(Connection& connection, const u8* data, u32 size);

    WorldPlayer* player_of(ClientId client);

    WorldServerConfig config_;
    World world_;
    ConnectionServer connections_;
    ShardCoordinator coordinator_;
    HttpServer http_;
    WebSocketServer viewer_;

    std::unordered_map<ClientId, WorldPlayer> players_;
    std::unordered_map<std::string, ClientId> by_player_id_;
    std::unordered_map<std::string, GhostState> ghosts_;

    struct PendingAdmit {
        ClientId client = 0;
        ConnectCredential credential;
        MigrationTicket ticket;
        bool has_ticket = false;
        u32 region = 0;
        u64 expires_ms = 0;
    };
    std::unordered_map<ClientId, PendingAdmit> pending_admits_;
    std::unordered_map<std::string, ClientId> pending_by_player_;
    std::unordered_map<u64, MigrationTicket> redeemed_tickets_;

    WorldServerStats stats_;
    std::vector<u8> scratch_;
    std::atomic<bool> running_{false};
    ClientId next_client_ = 1;
    u64 last_presence_refresh_ms_ = 0;
    u64 last_viewer_publish_us_ = 0;

    Histogram* tick_histogram_ = nullptr;
    Histogram* step_histogram_ = nullptr;
    Histogram* replicate_histogram_ = nullptr;
    Histogram* encode_histogram_ = nullptr;
    Histogram* send_histogram_ = nullptr;
    Histogram* net_histogram_ = nullptr;
    Histogram* snapshot_histogram_ = nullptr;
    Counter* snapshots_counter_ = nullptr;
    Counter* snapshot_bytes_counter_ = nullptr;
    Counter* inputs_counter_ = nullptr;
    Counter* migrations_out_counter_ = nullptr;
    Counter* migrations_in_counter_ = nullptr;
    Gauge* players_gauge_ = nullptr;
    Gauge* entities_gauge_ = nullptr;
};

}  // namespace morton
