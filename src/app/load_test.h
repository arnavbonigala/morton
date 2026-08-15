#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "app/game_client.h"
#include "metrics/histogram.h"

namespace morton {

struct LoadTestConfig {
    Address matchmaker;
    Address direct_shard;
    std::string player_prefix = "bot";
    u32 clients = 500;
    u32 threads = 4;
    u32 ramp_per_second = 200;
    u32 duration_seconds = 20;
    u32 report_interval_ms = 5000;
    u32 seed = 0x9e3779b9u;
    u64 timeout_us = 5000000;

    /// Seconds a dropped bot waits before asking the matchmaker for a new
    /// session. Zero leaves dropped bots down, which is what a pure throughput
    /// run wants; a chaos run wants them to come back.
    u32 rejoin_backoff_seconds = 2;
    bool quiet = false;
    WorldParams params;
};

struct LoadTestReport {
    u32 clients_requested = 0;
    u32 clients_connected = 0;
    u32 clients_peak_connected = 0;
    u32 clients_failed = 0;
    u64 inputs_sent = 0;
    u64 snapshots_received = 0;
    u64 snapshots_applied = 0;
    u64 migrations = 0;
    u64 rejoins = 0;
    u64 bytes_sent = 0;
    u64 bytes_received = 0;
    f64 wall_seconds = 0.0;
    f64 rtt_p50_ms = 0.0;
    f64 rtt_p95_ms = 0.0;
    f64 rtt_p99_ms = 0.0;
    f64 rtt_max_ms = 0.0;
    f64 loss_mean_percent = 0.0;
    f64 loss_p99_percent = 0.0;
    f64 client_recv_kbps_mean = 0.0;
    f64 client_recv_kbps_p99 = 0.0;
    f64 client_send_kbps_mean = 0.0;
    f64 prediction_error_mean = 0.0;
    f64 prediction_error_p99 = 0.0;
    f64 downstream_kbps_total = 0.0;
    f64 upstream_kbps_total = 0.0;
    f64 snapshots_per_second = 0.0;
};

/// A fleet of real clients driven against a live cluster.
///
/// Every bot is a full GameClient: real handshake, real prediction and
/// reconciliation, real migration handling. Nothing about the traffic is
/// synthetic, so the numbers this reports are the numbers a real player sees.
///
/// Bots are spread over worker threads, each thread stepping its slice at the
/// world tick rate. Clients join on a ramp so a large fleet does not arrive as
/// a single connect storm.
class LoadTest {
public:
    bool run(const LoadTestConfig& config, LoadTestReport* out);
    void request_stop() { stop_.store(true, std::memory_order_relaxed); }

private:
    struct Bot {
        GameClient client;
        u64 join_at_us = 0;
        bool started = false;
        bool failed = false;
        f32 heading = 0.f;
        f32 turn_at = 0.f;
        u64 rejoin_at_us = 0;
        u32 rejoins = 0;
    };

    void run_worker(u32 worker, u32 first, u32 count);
    bool launch(Bot& bot, u32 index);
    void sample(Bot& bot);

    LoadTestConfig config_;
    std::vector<std::unique_ptr<Bot>> bots_;
    std::atomic<bool> stop_{false};
    std::atomic<u32> connected_{0};
    std::atomic<u32> peak_connected_{0};
    std::atomic<u32> failed_{0};
    std::atomic<u64> rejoins_{0};
    std::vector<std::atomic<u32>> worker_live_;

    Histogram rtt_;
    Histogram loss_;
    Histogram recv_kbps_;
    Histogram send_kbps_;
    Histogram prediction_error_;
    u64 started_at_us_ = 0;
    u64 deadline_us_ = 0;
};

std::string report_json(const LoadTestReport& report);
std::string report_text(const LoadTestReport& report);

}  // namespace morton
