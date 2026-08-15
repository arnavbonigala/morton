#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "core/build_info.h"
#include "core/time.h"
#include "metrics/histogram.h"
#include "proto/snapshot.h"
#include "sim/world.h"

using namespace morton;

namespace {

const char* option(int argc, char** argv, const char* name, const char* fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    }
    const char* env = std::getenv(name + 2);
    return env != nullptr ? env : fallback;
}

u32 number(int argc, char** argv, const char* name, const char* fallback) {
    return static_cast<u32>(std::strtoul(option(argc, argv, name, fallback), nullptr, 10));
}

bool flag(int argc, char** argv, const char* name) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) return true;
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    u32 players = number(argc, argv, "--MORTON_PLAYERS", "350");
    u32 drifters = number(argc, argv, "--MORTON_DRIFTERS", "300");
    u32 ticks = number(argc, argv, "--MORTON_TICKS", "600");
    u32 warmup = number(argc, argv, "--MORTON_WARMUP", "60");
    u32 seed = number(argc, argv, "--MORTON_SEED", "12345");
    u32 cells = number(argc, argv, "--MORTON_CELLS", "64");
    bool json = flag(argc, argv, "--json");

    WorldParams params;
    params.size = 2048.f;
    params.tick_rate = 30;

    World world;
    world.configure(params, cells);

    std::mt19937 rng(seed);
    std::uniform_real_distribution<f32> place(0.f, params.size);
    std::uniform_real_distribution<f32> axis(-1.f, 1.f);

    InterestConfig interest;

    struct Viewer {
        ClientId client;
        EntityId entity;
        ReplicationState replication;
        Tick pending_tick = 0;
        bool has_pending = false;
    };

    std::vector<std::unique_ptr<Viewer>> viewers;
    viewers.reserve(players);
    for (u32 i = 0; i < players; ++i) {
        auto viewer = std::make_unique<Viewer>();
        viewer->client = i + 1;
        viewer->entity = world.spawn_player(viewer->client, Vec2(place(rng), place(rng)));
        viewer->replication.configure(interest, params);
        viewers.push_back(std::move(viewer));
    }
    for (u32 i = 0; i < drifters; ++i) world.spawn_drifter(Vec2(place(rng), place(rng)));

    std::vector<u8> packet(4096);
    Histogram replicate_ms, snapshot_bytes, entities_sent;
    u64 total_bytes = 0;
    u64 snapshots = 0;

    for (u32 tick = 0; tick < warmup + ticks; ++tick) {
        for (auto& viewer : viewers) {
            MoveInput input;
            input.sequence = tick + 1;
            input.tick = tick;
            input.move_x = axis(rng);
            input.move_y = axis(rng);
            input.sprint = (tick & 7u) == 0;
            quantize_input(&input);
            world.queue_input(viewer->client, input);
        }

        world.step();

        bool measured = tick >= warmup;
        u64 started = now_us();
        for (auto& viewer : viewers) {
            if (viewer->has_pending) viewer->replication.acknowledge(viewer->pending_tick);
            u32 size = viewer->replication.encode(world, viewer->entity, tick + 1, packet.data(),
                                                  static_cast<u32>(packet.size()));
            if (size == 0) continue;
            viewer->pending_tick = world.tick();
            viewer->has_pending = true;
            if (!measured) continue;
            total_bytes += size;
            ++snapshots;
            snapshot_bytes.record(static_cast<f64>(size));
            entities_sent.record(static_cast<f64>(viewer->replication.stats().entities_sent));
        }
        if (measured) replicate_ms.record(static_cast<f64>(now_us() - started) / 1000.0);
    }

    f64 per_viewer_us =
        snapshots == 0 ? 0.0 : replicate_ms.sum() * 1000.0 / static_cast<f64>(snapshots);
    f64 kbps = static_cast<f64>(total_bytes) * 8.0 * params.tick_rate /
               (static_cast<f64>(ticks) * 1000.0) / static_cast<f64>(players);

    if (json) {
        std::printf(
            "{\"players\":%u,\"drifters\":%u,\"ticks\":%u,"
            "\"replicate_mean_ms\":%.4f,\"replicate_p50_ms\":%.4f,\"replicate_p99_ms\":%.4f,"
            "\"replicate_max_ms\":%.4f,\"per_viewer_us\":%.3f,\"snapshot_mean_bytes\":%.1f,"
            "\"entities_sent_mean\":%.1f,\"per_client_kbps\":%.1f}\n",
            players, drifters, ticks, replicate_ms.mean(), replicate_ms.p50(), replicate_ms.p99(),
            replicate_ms.max(), per_viewer_us, snapshot_bytes.mean(), entities_sent.mean(), kbps);
        return 0;
    }

    std::printf("%s\n", BuildInfo::summary().c_str());
    std::printf("players %u  drifters %u  measured ticks %u\n", players, drifters, ticks);
    std::printf("replicate per tick   mean %.3f ms  p50 %.3f  p99 %.3f  max %.3f\n",
                replicate_ms.mean(), replicate_ms.p50(), replicate_ms.p99(), replicate_ms.max());
    std::printf("per viewer           %.2f us\n", per_viewer_us);
    std::printf("snapshot             mean %.1f bytes  entities %.1f  per-client %.1f kbit/s\n",
                snapshot_bytes.mean(), entities_sent.mean(), kbps);
    return 0;
}
