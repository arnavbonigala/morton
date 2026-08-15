#include <signal.h>

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "cluster/matchmaker.h"
#include "core/build_info.h"
#include "core/log.h"
#include "core/time.h"

namespace {

std::atomic<bool> g_running{true};

void handle_signal(int) { g_running.store(false, std::memory_order_relaxed); }

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

    MatchmakerConfig config;
    config.key_prefix = option(argc, argv, "--MORTON_PREFIX", "morton");
    config.session_ttl_ms = static_cast<u32>(
        std::strtoul(option(argc, argv, "--MORTON_SESSION_TTL_MS", "15000"), nullptr, 10));
    config.regions.regions_per_axis = static_cast<u32>(
        std::strtoul(option(argc, argv, "--MORTON_REGIONS_PER_AXIS", "2"), nullptr, 10));

    if (!Address::parse(option(argc, argv, "--MORTON_HTTP", "0.0.0.0:8080"), &config.http_bind) ||
        !Address::parse(option(argc, argv, "--MORTON_REDIS", "127.0.0.1:6379"), &config.redis)) {
        MORTON_LOG_ERROR("could not parse --MORTON_HTTP or --MORTON_REDIS");
        return 1;
    }

    MORTON_LOG_INFO("%s", BuildInfo::summary().c_str());

    Matchmaker matchmaker;
    if (!matchmaker.start(config)) return 1;

    ::signal(SIGINT, handle_signal);
    ::signal(SIGTERM, handle_signal);

    while (g_running.load(std::memory_order_relaxed)) sleep_us(200000);

    MORTON_LOG_INFO("matchmaker stopping after %llu grants and %llu reconnects",
                    static_cast<unsigned long long>(matchmaker.grants()),
                    static_cast<unsigned long long>(matchmaker.reconnects()));
    matchmaker.stop();
    return 0;
}
