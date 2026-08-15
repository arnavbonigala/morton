#include <signal.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "app/load_test.h"
#include "core/build_info.h"
#include "core/log.h"

namespace {

morton::LoadTest* g_load = nullptr;

void handle_signal(int) {
    if (g_load != nullptr) g_load->request_stop();
}

const char* option(int argc, char** argv, const char* name, const char* fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    }
    const char* env = std::getenv(name + 2);
    return env != nullptr ? env : fallback;
}

bool flag(int argc, char** argv, const char* name) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) return true;
    }
    return false;
}

morton::u32 number(int argc, char** argv, const char* name, const char* fallback) {
    return static_cast<morton::u32>(std::strtoul(option(argc, argv, name, fallback), nullptr, 10));
}

}  // namespace

int main(int argc, char** argv) {
    using namespace morton;

    LoadTestConfig config;
    config.player_prefix = option(argc, argv, "--MORTON_PREFIX", "bot");
    config.clients = number(argc, argv, "--MORTON_CLIENTS", "500");
    config.threads = number(argc, argv, "--MORTON_THREADS", "4");
    config.ramp_per_second = number(argc, argv, "--MORTON_RAMP", "200");
    config.duration_seconds = number(argc, argv, "--MORTON_DURATION", "20");
    config.report_interval_ms = number(argc, argv, "--MORTON_REPORT_MS", "5000");
    config.params.tick_rate = number(argc, argv, "--MORTON_TICK_RATE", "30");

    const char* direct = option(argc, argv, "--MORTON_SHARD", "");
    if (direct[0] != '\0' && !Address::parse(direct, &config.direct_shard)) {
        MORTON_LOG_ERROR("could not parse --MORTON_SHARD");
        return 1;
    }
    if (!Address::parse(option(argc, argv, "--MORTON_MATCHMAKER", "127.0.0.1:8080"),
                        &config.matchmaker)) {
        MORTON_LOG_ERROR("could not parse --MORTON_MATCHMAKER");
        return 1;
    }

    MORTON_LOG_INFO("%s", BuildInfo::summary().c_str());

    LoadTest load;
    g_load = &load;
    ::signal(SIGINT, handle_signal);
    ::signal(SIGTERM, handle_signal);

    LoadTestReport report;
    if (!load.run(config, &report)) {
        MORTON_LOG_ERROR("load test failed to start");
        return 1;
    }

    if (flag(argc, argv, "--json")) {
        std::printf("%s\n", report_json(report).c_str());
    } else {
        std::printf("\n%s", report_text(report).c_str());
    }
    return report.clients_connected == 0 ? 1 : 0;
}
