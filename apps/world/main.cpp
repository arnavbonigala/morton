#include <signal.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include "app/world_server.h"
#include "core/build_info.h"
#include "core/log.h"

namespace {

morton::WorldServer* g_server = nullptr;

void handle_signal(int) {
    if (g_server != nullptr) g_server->request_stop();
}

const char* option(int argc, char** argv, const char* name, const char* fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    }
    const char* env = std::getenv(name + 2);
    return env != nullptr ? env : fallback;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace morton;

    WorldServerConfig config;
    config.shard_id = option(argc, argv, "--MORTON_SHARD_ID", "world-a");
    config.key_prefix = option(argc, argv, "--MORTON_PREFIX", "morton");
    config.capacity = static_cast<u32>(
        std::strtoul(option(argc, argv, "--MORTON_CAPACITY", "512"), nullptr, 10));
    config.drifters = static_cast<u32>(
        std::strtoul(option(argc, argv, "--MORTON_DRIFTERS", "200"), nullptr, 10));
    config.params.tick_rate = static_cast<u32>(
        std::strtoul(option(argc, argv, "--MORTON_TICK_RATE", "30"), nullptr, 10));
    config.regions.regions_per_axis = static_cast<u32>(
        std::strtoul(option(argc, argv, "--MORTON_REGIONS_PER_AXIS", "2"), nullptr, 10));
    config.regions.world_size = config.params.size;
    config.advertise_udp = option(argc, argv, "--MORTON_ADVERTISE_UDP", "");
    config.advertise_http = option(argc, argv, "--MORTON_ADVERTISE_HTTP", "");
    config.advertise =
        std::strcmp(option(argc, argv, "--MORTON_ADVERTISE", "1"), "0") != 0;

    const char* ws = option(argc, argv, "--MORTON_WS", "");
    if (ws[0] != '\0' && !Address::parse(ws, &config.ws_bind)) {
        MORTON_LOG_ERROR("could not parse --MORTON_WS");
        return 1;
    }
    if (ws[0] != '\0') {
        config.viewer_hz = static_cast<u32>(
            std::strtoul(option(argc, argv, "--MORTON_VIEWER_HZ", "10"), nullptr, 10));
    }

    if (!Address::parse(option(argc, argv, "--MORTON_UDP", "0.0.0.0:40000"), &config.udp_bind) ||
        !Address::parse(option(argc, argv, "--MORTON_HTTP", "0.0.0.0:40080"), &config.http_bind) ||
        !Address::parse(option(argc, argv, "--MORTON_REDIS", "127.0.0.1:6379"), &config.redis)) {
        MORTON_LOG_ERROR("could not parse one of --MORTON_UDP/--MORTON_HTTP/--MORTON_REDIS");
        return 1;
    }

    MORTON_LOG_INFO("%s", BuildInfo::summary().c_str());

    WorldServer server;
    if (!server.start(config)) {
        MORTON_LOG_ERROR("shard %s failed to start", config.shard_id.c_str());
        return 1;
    }

    g_server = &server;
    ::signal(SIGINT, handle_signal);
    ::signal(SIGTERM, handle_signal);

    server.run();
    server.stop();
    MORTON_LOG_INFO("shard %s stopped after %llu ticks", config.shard_id.c_str(),
                    static_cast<unsigned long long>(server.stats().ticks));
    return 0;
}
