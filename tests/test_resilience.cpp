#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "app/game_client.h"
#include "app/load_test.h"
#include "check.h"
#include "live_cluster.h"

using namespace morton;
using morton_test::LiveCluster;
using morton_test::redis_fixture;
using morton_test::test_regions;
using morton_test::wipe;

namespace {

bool join_one(const std::string& player_id, const Address& matchmaker, u64 budget_us) {
    GameClientConfig config;
    config.player_id = player_id;
    config.matchmaker = matchmaker;

    GameClient client;
    if (!client.start(config)) return false;
    if (!client.request_session()) {
        client.stop();
        return false;
    }

    u64 deadline = now_us() + budget_us;
    while (now_us() < deadline) {
        client.update(now_us());
        if (client.connected() && client.local_entity() != kInvalidEntity) {
            client.stop();
            return true;
        }
        sleep_us(2000);
    }
    client.stop();
    return false;
}

}  // namespace

TEST_CASE(a_running_fleet_is_unaffected_by_a_redis_outage) {
    wipe("morton:r1");

    LiveCluster cluster;
    CHECK(cluster.start("morton:r1", {"world-a", "world-b"}, 60));

    LoadTestConfig config;
    config.matchmaker = cluster.matchmaker.http_address();
    config.player_prefix = "outage";
    config.clients = 40;
    config.threads = 2;
    config.ramp_per_second = 400;
    config.duration_seconds = 26;
    config.report_interval_ms = 3000;
    config.rejoin_backoff_seconds = 1;
    config.quiet = true;

    std::atomic<bool> watching{true};
    std::atomic<bool> outage{false};
    std::atomic<u32> residents_during_outage{~0u};
    std::atomic<u32> residents_after_recovery{0};

    std::thread chaos([&] {
        sleep_us(7000000);
        redis_fixture().stop();
        outage.store(true, std::memory_order_relaxed);
        sleep_us(6000000);
        outage.store(false, std::memory_order_relaxed);
        CHECK(redis_fixture().spawn());
    });

    // The shards must keep simulating the players they already hold while the
    // coordination store is unreachable; only new sessions and handoffs depend
    // on it.
    std::thread watcher([&] {
        bool recovered = false;
        while (watching.load(std::memory_order_relaxed)) {
            u32 resident = cluster.residents();
            if (outage.load(std::memory_order_relaxed)) {
                recovered = true;
                u32 seen = residents_during_outage.load(std::memory_order_relaxed);
                while (resident < seen &&
                       !residents_during_outage.compare_exchange_weak(seen, resident)) {
                }
            } else if (recovered) {
                u32 seen = residents_after_recovery.load(std::memory_order_relaxed);
                while (resident > seen &&
                       !residents_after_recovery.compare_exchange_weak(seen, resident)) {
                }
            }
            sleep_us(100000);
        }
    });

    LoadTest load;
    LoadTestReport report;
    CHECK(load.run(config, &report));
    watching.store(false, std::memory_order_relaxed);
    watcher.join();
    chaos.join();

    // A player mid-handoff when the store dies cannot complete it until the store
    // is back, so the run is judged on the fleet being whole again afterwards
    // rather than on the count at whatever instant the run happens to end.
    CHECK_EQ(residents_during_outage.load(std::memory_order_relaxed), config.clients);
    CHECK_EQ(residents_after_recovery.load(std::memory_order_relaxed), config.clients);
    CHECK(report.loss_mean_percent < 1.0);
    CHECK(report.snapshots_applied > 0);

    // Presence is written with a TTL every refresh, so an empty store heals on
    // its own rather than needing an operator to re-register the shards.
    bool relisted = false;
    u64 deadline = now_us() + 15000000;
    while (now_us() < deadline) {
        if (cluster.matchmaker.shards().size() == cluster.servers.size()) {
            relisted = true;
            break;
        }
        sleep_us(200000);
    }
    CHECK(relisted);
    CHECK(join_one("after-outage", cluster.matchmaker.http_address(), 5000000));

    std::printf("       survived a 6 s redis outage: %u players held, %.2f%% loss, "
                "cluster relisted\n",
                config.clients, report.loss_mean_percent);

    cluster.shutdown();
}

int main() {
    if (!redis_fixture().start()) {
        std::printf("redis-server unavailable; skipping resilience tests\n");
        return 0;
    }
    int result = ::morton_test::run_all();
    redis_fixture().stop();
    return result;
}
