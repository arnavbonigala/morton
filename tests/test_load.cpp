#include <atomic>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "app/load_test.h"
#include "app/world_server.h"
#include "check.h"
#include "cluster/matchmaker.h"
#include "core/time.h"
#include "redis_fixture.h"

using namespace morton;
using morton_test::redis_fixture;

namespace {

RegionMap test_regions() {
    RegionMap map;
    map.world_size = 2048.f;
    map.regions_per_axis = 2;
    map.margin = 48.f;
    return map;
}

void wipe(const std::string& prefix) {
    RedisClient client;
    client.connect(redis_fixture().address());
    RedisReply keys = client.command({"KEYS", prefix + ":*"});
    if (keys.type != RedisType::kArray || keys.elements.empty()) return;
    std::vector<std::string> args{"DEL"};
    for (const RedisReply& key : keys.elements) args.push_back(key.str);
    client.command(args);
}

/// A cluster whose shards tick on their own threads, so the fleet under test
/// talks to servers that are genuinely running rather than being pumped by the
/// test loop.
struct LiveCluster {
    std::vector<std::unique_ptr<WorldServer>> servers;
    std::vector<std::thread> threads;
    std::set<std::size_t> dead;
    Matchmaker matchmaker;

    bool start(const std::string& prefix, const std::vector<std::string>& shard_ids, u32 drifters) {
        for (const std::string& id : shard_ids) {
            WorldServerConfig config;
            config.shard_id = id;
            config.udp_bind = Address(0x7f000001u, 0);
            config.http_bind = Address(0x7f000001u, 0);
            config.redis = redis_fixture().address();
            config.key_prefix = prefix;
            config.regions = test_regions();
            config.capacity = 4096;
            config.max_connections = 8192;
            config.drifters = drifters;
            auto server = std::make_unique<WorldServer>();
            if (!server->start(config)) return false;
            servers.push_back(std::move(server));
        }

        MatchmakerConfig config;
        config.redis = redis_fixture().address();
        config.http_bind = Address(0x7f000001u, 0);
        config.key_prefix = prefix;
        config.session_ttl_ms = 60000;
        config.regions = test_regions();
        if (!matchmaker.start(config)) return false;

        for (auto& server : servers) {
            server->coordinator().refresh(wall_ms(), 0, 0.0, true);
        }
        for (auto& server : servers) {
            WorldServer* raw = server.get();
            threads.emplace_back([raw] { raw->run(); });
        }
        return true;
    }

    void kill(std::size_t index) {
        servers[index]->request_stop();
        threads[index].join();
        servers[index]->stop();
        dead.insert(index);
    }

    void shutdown() {
        for (std::size_t i = 0; i < servers.size(); ++i) {
            if (dead.count(i) == 0) servers[i]->request_stop();
        }
        for (std::size_t i = 0; i < threads.size(); ++i) {
            if (dead.count(i) == 0) threads[i].join();
        }
        threads.clear();
        matchmaker.stop();
        for (std::size_t i = 0; i < servers.size(); ++i) {
            if (dead.count(i) == 0) servers[i]->stop();
        }
        servers.clear();
    }

    u32 residents() const {
        u32 total = 0;
        for (const auto& server : servers) total += server->resident_player_count();
        return total;
    }
};

}  // namespace

TEST_CASE(a_fleet_of_clients_all_connect_and_stay_connected_under_load) {
    wipe("morton:l1");

    LiveCluster cluster;
    CHECK(cluster.start("morton:l1", {"world-a"}, 150));

    LoadTestConfig config;
    config.matchmaker = cluster.matchmaker.http_address();
    config.player_prefix = "fleet";
    config.clients = 120;
    config.threads = 4;
    config.ramp_per_second = 400;
    config.duration_seconds = 6;
    config.report_interval_ms = 2000;
    config.quiet = true;

    LoadTest load;
    LoadTestReport report;
    CHECK(load.run(config, &report));

    CHECK_EQ(report.clients_peak_connected, config.clients);
    CHECK_EQ(report.clients_failed, 0u);
    CHECK(report.snapshots_applied > config.clients * 30ull);
    CHECK(report.inputs_sent > config.clients * 30ull);
    CHECK(report.rtt_p99_ms > 0.0);
    CHECK(report.rtt_p99_ms < 200.0);
    CHECK(report.loss_p99_percent < 25.0);

    f64 tick_p99 = cluster.servers[0]->tick_p99_ms();
    CHECK(tick_p99 > 0.0);
    CHECK(tick_p99 < 33.0);

    std::printf("       %u clients, rtt p50/p99 %.2f/%.2f ms, %.1f kbit/s per client, "
                "shard tick p99 %.2f ms\n",
                report.clients_peak_connected, report.rtt_p50_ms, report.rtt_p99_ms,
                report.client_recv_kbps_mean, tick_p99);

    cluster.shutdown();
}

TEST_CASE(measured_per_client_bandwidth_stays_inside_the_replication_budget) {
    wipe("morton:l2");

    LiveCluster cluster;
    CHECK(cluster.start("morton:l2", {"world-a"}, 400));

    LoadTestConfig config;
    config.matchmaker = cluster.matchmaker.http_address();
    config.player_prefix = "band";
    config.clients = 80;
    config.threads = 4;
    config.ramp_per_second = 400;
    config.duration_seconds = 6;
    config.report_interval_ms = 1500;
    config.quiet = true;

    LoadTest load;
    LoadTestReport report;
    CHECK(load.run(config, &report));
    CHECK_EQ(report.clients_peak_connected, config.clients);

    InterestConfig interest;
    f64 budget_kbps = static_cast<f64>(interest.max_snapshot_bytes) * 8.0 *
                      static_cast<f64>(config.params.tick_rate) / 1000.0;
    CHECK(report.client_recv_kbps_mean > 0.0);
    CHECK(report.client_recv_kbps_mean < budget_kbps);
    CHECK(report.client_recv_kbps_p99 < budget_kbps);

    u64 entities = cluster.servers[0]->world().entities().size();
    f64 uncompressed_kbps = static_cast<f64>(entities) * 20.0 * 8.0 *
                            static_cast<f64>(config.params.tick_rate) / 1000.0;
    f64 reduction = 100.0 * (1.0 - report.client_recv_kbps_mean / uncompressed_kbps);
    CHECK(reduction > 80.0);

    std::printf("       %llu entities: %.1f kbit/s per client vs %.1f uncompressed "
                "(%.2f%% reduction, budget %.1f)\n",
                static_cast<unsigned long long>(entities), report.client_recv_kbps_mean,
                uncompressed_kbps, reduction, budget_kbps);

    cluster.shutdown();
}

TEST_CASE(a_roaming_fleet_migrates_across_shards_without_losing_players) {
    wipe("morton:l3");

    LiveCluster cluster;
    CHECK(cluster.start("morton:l3", {"world-a", "world-b"}, 100));

    LoadTestConfig config;
    config.matchmaker = cluster.matchmaker.http_address();
    config.player_prefix = "roam";
    config.clients = 60;
    config.threads = 4;
    config.ramp_per_second = 400;
    config.duration_seconds = 10;
    config.report_interval_ms = 2000;
    config.quiet = true;

    std::atomic<bool> watching{true};
    std::atomic<u32> peak_residents{0};
    std::thread watcher([&] {
        while (watching.load(std::memory_order_relaxed)) {
            u32 residents = cluster.residents();
            u32 seen = peak_residents.load(std::memory_order_relaxed);
            while (residents > seen &&
                   !peak_residents.compare_exchange_weak(seen, residents,
                                                         std::memory_order_relaxed)) {
            }
            sleep_us(20000);
        }
    });

    LoadTest load;
    LoadTestReport report;
    CHECK(load.run(config, &report));
    watching.store(false, std::memory_order_relaxed);
    watcher.join();

    CHECK_EQ(report.clients_peak_connected, config.clients);
    CHECK(report.migrations > 0);

    u64 migrations_out = 0;
    u64 migrations_in = 0;
    for (const auto& server : cluster.servers) {
        migrations_out += server->stats().migrations_out;
        migrations_in += server->stats().migrations_in;
    }
    CHECK(migrations_in <= migrations_out);
    CHECK(migrations_out - migrations_in <= 2);
    CHECK(report.migrations <= migrations_out);
    // A player being handed over must never be authoritative on two shards at
    // once, so the residency total can reach the fleet size but never exceed it.
    CHECK_EQ(peak_residents.load(std::memory_order_relaxed), config.clients);

    std::printf("       %llu handoffs across 2 shards, peak %u players resident, "
                "prediction error mean %.3f units\n",
                static_cast<unsigned long long>(report.migrations),
                peak_residents.load(std::memory_order_relaxed), report.prediction_error_mean);

    cluster.shutdown();
}

TEST_CASE(players_on_a_shard_that_dies_are_replaced_onto_the_survivor) {
    wipe("morton:l4");

    LiveCluster cluster;
    CHECK(cluster.start("morton:l4", {"world-a", "world-b"}, 60));

    LoadTestConfig config;
    config.matchmaker = cluster.matchmaker.http_address();
    config.player_prefix = "survivor";
    config.clients = 40;
    config.threads = 2;
    config.ramp_per_second = 400;
    config.duration_seconds = 26;
    config.report_interval_ms = 3000;
    config.rejoin_backoff_seconds = 1;
    config.quiet = true;

    std::atomic<bool> killed{false};
    std::thread chaos([&] {
        sleep_us(6000000);
        cluster.kill(1);
        killed.store(true, std::memory_order_relaxed);
    });

    LoadTest load;
    LoadTestReport report;
    CHECK(load.run(config, &report));
    chaos.join();
    CHECK(killed.load(std::memory_order_relaxed));

    CHECK(report.rejoins > 0);
    CHECK_EQ(report.clients_connected, config.clients);
    CHECK_EQ(cluster.servers[0]->resident_player_count(), config.clients);

    std::vector<ShardInfo> live = cluster.matchmaker.shards();
    CHECK_EQ(static_cast<u32>(live.size()), 1u);
    CHECK(live[0].id == "world-a");

    std::printf("       shard died mid-run: %llu rejoins, all %u players resident on the "
                "survivor\n",
                static_cast<unsigned long long>(report.rejoins), report.clients_connected);

    cluster.shutdown();
}

int main() {
    if (!redis_fixture().start()) {
        std::printf("redis-server unavailable; skipping load tests\n");
        return 0;
    }
    int result = ::morton_test::run_all();
    redis_fixture().stop();
    return result;
}
