#include "app/load_test.h"

#include <cmath>
#include <cstdio>
#include <thread>

#include "core/log.h"
#include "core/time.h"

namespace morton {
namespace {

u32 mix32(u32 value) {
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

f32 unit_random(u32 value) {
    return static_cast<f32>(mix32(value) & 0xffffffu) / static_cast<f32>(0xffffffu);
}

}  // namespace

void LoadTest::sample(Bot& bot) {
    if (!bot.started || bot.failed || !bot.client.connected()) return;

    const NetworkStats& net = bot.client.net_stats();
    if (net.rtt_ms > 0.f) rtt_.record(static_cast<f64>(net.rtt_ms));
    loss_.record(static_cast<f64>(net.sent_loss_percent));
    recv_kbps_.record(static_cast<f64>(net.recv_kbps));
    send_kbps_.record(static_cast<f64>(net.sent_kbps));

    const ClientViewStats& view = bot.client.view().stats();
    if (view.snapshots_applied > 0) {
        prediction_error_.record(static_cast<f64>(view.last_prediction_error));
    }
}

void LoadTest::run_worker(u32 worker, u32 first, u32 count) {
    const u64 step_us = 1000000ull / config_.params.tick_rate;
    const u64 sample_interval_us = static_cast<u64>(config_.report_interval_ms) * 1000ull;
    u64 next_step = now_us();
    u64 next_sample = next_step + sample_interval_us;

    while (!stop_.load(std::memory_order_relaxed)) {
        u64 now = now_us();
        if (now >= deadline_us_) break;

        for (u32 i = 0; i < count; ++i) {
            Bot& bot = *bots_[first + i];

            if (!bot.started) {
                if (now < bot.join_at_us) continue;
                bot.started = true;

                GameClientConfig client;
                client.player_id = config_.player_prefix + "-" + std::to_string(first + i);
                client.matchmaker = config_.matchmaker;
                client.direct_shard = config_.direct_shard;
                client.params = config_.params;
                client.timeout_us = config_.timeout_us;
                if (!bot.client.start(client)) {
                    bot.failed = true;
                    failed_.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                u32 seed = mix32(config_.seed ^ (first + i));
                bot.heading = unit_random(seed) * 6.2831853f;
                bot.turn_at = 1.f + unit_random(seed + 1) * 3.f;
                continue;
            }
            if (bot.failed) continue;

            bot.client.update(now);
            if (!bot.client.connected()) continue;

            f32 elapsed = static_cast<f32>(now - started_at_us_) / 1000000.f;
            if (elapsed > bot.turn_at) {
                u32 seed = mix32(config_.seed ^ ((first + i) << 8) ^
                                 static_cast<u32>(elapsed));
                bot.heading += (unit_random(seed) - 0.5f) * 2.4f;
                bot.turn_at = elapsed + 1.f + unit_random(seed + 7) * 3.f;
            }

            Vec2 axis{std::cos(bot.heading), std::sin(bot.heading)};
            bot.client.send_input(axis, false, now);
        }

        now = now_us();
        if (now >= next_sample) {
            for (u32 i = 0; i < count; ++i) sample(*bots_[first + i]);
            next_sample = now + sample_interval_us;
            if (worker == 0 && !config_.quiet) {
                MORTON_LOG_INFO("load: %u connected, rtt p99 %.2f ms, recv %.1f kbit/s",
                                connected_.load(std::memory_order_relaxed), rtt_.p99(),
                                recv_kbps_.mean());
            }
        }

        u32 live = 0;
        for (u32 i = 0; i < count; ++i) {
            if (bots_[first + i]->started && bots_[first + i]->client.connected()) ++live;
        }
        u32 previous = worker_live_[worker].exchange(live, std::memory_order_relaxed);
        u32 total = connected_.fetch_add(live - previous, std::memory_order_relaxed) +
                    live - previous;
        u32 peak = peak_connected_.load(std::memory_order_relaxed);
        while (total > peak && !peak_connected_.compare_exchange_weak(
                                  peak, total, std::memory_order_relaxed)) {
        }

        next_step += step_us;
        if (next_step > now) precise_sleep_until(next_step);
        else next_step = now;
    }

    for (u32 i = 0; i < count; ++i) sample(*bots_[first + i]);
}

bool LoadTest::run(const LoadTestConfig& config, LoadTestReport* out) {
    if (config.clients == 0 || out == nullptr) return false;
    if (!config.matchmaker.valid() && !config.direct_shard.valid()) return false;

    config_ = config;
    if (config_.threads == 0) config_.threads = 1;
    if (config_.threads > config_.clients) config_.threads = config_.clients;
    if (config_.ramp_per_second == 0) config_.ramp_per_second = config_.clients;
    if (config_.params.tick_rate == 0) config_.params.tick_rate = 30;

    stop_.store(false, std::memory_order_relaxed);
    connected_.store(0, std::memory_order_relaxed);
    peak_connected_.store(0, std::memory_order_relaxed);
    failed_.store(0, std::memory_order_relaxed);
    rtt_.reset();
    loss_.reset();
    recv_kbps_.reset();
    send_kbps_.reset();
    prediction_error_.reset();

    bots_.clear();
    bots_.reserve(config_.clients);
    started_at_us_ = now_us();
    for (u32 i = 0; i < config_.clients; ++i) {
        auto bot = std::make_unique<Bot>();
        bot->join_at_us = started_at_us_ +
                          (1000000ull * i) / config_.ramp_per_second;
        bots_.push_back(std::move(bot));
    }

    u64 ramp_us = (1000000ull * config_.clients) / config_.ramp_per_second;
    deadline_us_ = started_at_us_ + ramp_us +
                   static_cast<u64>(config_.duration_seconds) * 1000000ull;

    worker_live_ = std::vector<std::atomic<u32>>(config_.threads);
    for (std::atomic<u32>& live : worker_live_) live.store(0, std::memory_order_relaxed);

    std::vector<std::thread> workers;
    u32 per_worker = config_.clients / config_.threads;
    u32 remainder = config_.clients % config_.threads;
    u32 cursor = 0;
    for (u32 w = 0; w < config_.threads; ++w) {
        u32 count = per_worker + (w < remainder ? 1u : 0u);
        u32 first = cursor;
        cursor += count;
        workers.emplace_back([this, w, first, count] { run_worker(w, first, count); });
    }
    for (std::thread& worker : workers) worker.join();

    u64 finished_at = now_us();

    LoadTestReport report;
    report.clients_requested = config_.clients;
    report.wall_seconds = static_cast<f64>(finished_at - started_at_us_) / 1000000.0;

    for (auto& holder : bots_) {
        Bot& bot = *holder;
        if (bot.failed || !bot.started) continue;
        if (bot.client.connected()) ++report.clients_connected;

        const GameClientStats& stats = bot.client.stats();
        report.inputs_sent += stats.inputs_sent;
        report.snapshots_received += stats.snapshots_received;
        report.snapshots_applied += stats.snapshots_applied;
        report.migrations += stats.migrations;
        report.bytes_sent += stats.bytes_sent;
        report.bytes_received += stats.bytes_received;
    }
    report.clients_peak_connected = peak_connected_.load(std::memory_order_relaxed);
    report.clients_failed = failed_.load(std::memory_order_relaxed);

    report.rtt_p50_ms = rtt_.p50();
    report.rtt_p95_ms = rtt_.p95();
    report.rtt_p99_ms = rtt_.p99();
    report.rtt_max_ms = rtt_.max();
    report.loss_mean_percent = loss_.mean();
    report.loss_p99_percent = loss_.p99();
    report.client_recv_kbps_mean = recv_kbps_.mean();
    report.client_recv_kbps_p99 = recv_kbps_.p99();
    report.client_send_kbps_mean = send_kbps_.mean();
    report.prediction_error_mean = prediction_error_.mean();
    report.prediction_error_p99 = prediction_error_.p99();

    if (report.wall_seconds > 0.0) {
        report.downstream_kbps_total =
            static_cast<f64>(report.bytes_received) * 8.0 / 1000.0 / report.wall_seconds;
        report.upstream_kbps_total =
            static_cast<f64>(report.bytes_sent) * 8.0 / 1000.0 / report.wall_seconds;
        report.snapshots_per_second =
            static_cast<f64>(report.snapshots_received) / report.wall_seconds;
    }

    for (auto& holder : bots_) holder->client.stop();
    bots_.clear();

    *out = report;
    return true;
}

std::string report_json(const LoadTestReport& report) {
    char buffer[1600];
    int written = std::snprintf(
        buffer, sizeof(buffer),
        "{\"clients_requested\":%u,\"clients_connected\":%u,\"clients_peak_connected\":%u,"
        "\"clients_failed\":%u,"
        "\"wall_seconds\":%.3f,\"inputs_sent\":%llu,\"snapshots_received\":%llu,"
        "\"snapshots_applied\":%llu,\"migrations\":%llu,\"bytes_sent\":%llu,"
        "\"bytes_received\":%llu,\"rtt_p50_ms\":%.3f,\"rtt_p95_ms\":%.3f,"
        "\"rtt_p99_ms\":%.3f,\"rtt_max_ms\":%.3f,\"loss_mean_percent\":%.3f,"
        "\"loss_p99_percent\":%.3f,\"client_recv_kbps_mean\":%.2f,"
        "\"client_recv_kbps_p99\":%.2f,\"client_send_kbps_mean\":%.2f,"
        "\"prediction_error_mean\":%.4f,\"prediction_error_p99\":%.4f,"
        "\"downstream_kbps_total\":%.2f,\"upstream_kbps_total\":%.2f,"
        "\"snapshots_per_second\":%.1f}",
        report.clients_requested, report.clients_connected, report.clients_peak_connected,
        report.clients_failed, report.wall_seconds,
        static_cast<unsigned long long>(report.inputs_sent),
        static_cast<unsigned long long>(report.snapshots_received),
        static_cast<unsigned long long>(report.snapshots_applied),
        static_cast<unsigned long long>(report.migrations),
        static_cast<unsigned long long>(report.bytes_sent),
        static_cast<unsigned long long>(report.bytes_received), report.rtt_p50_ms,
        report.rtt_p95_ms, report.rtt_p99_ms, report.rtt_max_ms, report.loss_mean_percent,
        report.loss_p99_percent, report.client_recv_kbps_mean, report.client_recv_kbps_p99,
        report.client_send_kbps_mean, report.prediction_error_mean,
        report.prediction_error_p99, report.downstream_kbps_total,
        report.upstream_kbps_total, report.snapshots_per_second);
    return written > 0 ? std::string(buffer, static_cast<std::size_t>(written)) : std::string("{}");
}

std::string report_text(const LoadTestReport& report) {
    char buffer[1600];
    int written = std::snprintf(
        buffer, sizeof(buffer),
        "clients        %u/%u connected at exit, peak %u, %u never joined, %.1f s\n"
        "rtt            p50 %.2f ms  p95 %.2f ms  p99 %.2f ms  max %.2f ms\n"
        "packet loss    mean %.2f%%  p99 %.2f%%\n"
        "per client     down %.1f kbit/s (p99 %.1f)  up %.1f kbit/s\n"
        "aggregate      down %.1f kbit/s  up %.1f kbit/s  %.0f snapshots/s\n"
        "prediction     mean %.3f units  p99 %.3f units\n"
        "migrations     %llu  inputs %llu  snapshots %llu applied of %llu\n",
        report.clients_connected, report.clients_requested, report.clients_peak_connected,
        report.clients_failed, report.wall_seconds, report.rtt_p50_ms, report.rtt_p95_ms, report.rtt_p99_ms,
        report.rtt_max_ms, report.loss_mean_percent, report.loss_p99_percent,
        report.client_recv_kbps_mean, report.client_recv_kbps_p99,
        report.client_send_kbps_mean, report.downstream_kbps_total,
        report.upstream_kbps_total, report.snapshots_per_second,
        report.prediction_error_mean, report.prediction_error_p99,
        static_cast<unsigned long long>(report.migrations),
        static_cast<unsigned long long>(report.inputs_sent),
        static_cast<unsigned long long>(report.snapshots_applied),
        static_cast<unsigned long long>(report.snapshots_received));
    return written > 0 ? std::string(buffer, static_cast<std::size_t>(written)) : std::string();
}

}  // namespace morton
