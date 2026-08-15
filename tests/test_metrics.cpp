#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <thread>
#include <vector>

#include "check.h"
#include "http_client.h"
#include "metrics/registry.h"
#include "net/http.h"

using namespace morton;

namespace {

std::string http_get(const Address& server, const std::string& target,
                     const std::string& method = "GET", const std::string& body = "") {
    return morton_test::http_request(server, method, target, body);
}

std::string response_body(const std::string& response) {
    return morton_test::http_body(response);
}

Address loopback(u16 port) { return Address(0x7f000001u, port); }

}  // namespace

TEST_CASE(histogram_quantiles_match_a_known_distribution) {
    Histogram histogram;
    for (int i = 1; i <= 10000; ++i) histogram.record(static_cast<f64>(i) * 0.01);

    CHECK_EQ(histogram.count(), 10000u);

    f64 tolerance = 0.15;
    CHECK_NEAR(histogram.p50(), 50.0, 50.0 * tolerance);
    CHECK_NEAR(histogram.p95(), 95.0, 95.0 * tolerance);
    CHECK_NEAR(histogram.p99(), 99.0, 99.0 * tolerance);
    CHECK(histogram.p99() >= histogram.p95());
    CHECK(histogram.p95() >= histogram.p50());
    CHECK_NEAR(histogram.mean(), 50.005, 0.5);
    CHECK_NEAR(histogram.max(), 100.0, 0.01);
}

TEST_CASE(histogram_resolves_a_tail_hidden_inside_a_dense_body) {
    Histogram histogram;
    for (int i = 0; i < 9800; ++i) histogram.record(0.005);
    for (int i = 0; i < 200; ++i) histogram.record(0.400);

    CHECK(histogram.mean() < 0.020);
    CHECK_NEAR(histogram.p50(), 0.005, 0.002);
    CHECK(histogram.p99() > 0.100);
    CHECK_NEAR(histogram.p999(), 0.400, 0.060);
}

TEST_CASE(concurrent_recording_loses_no_samples) {
    Histogram histogram;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 20000;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&histogram, t] {
            for (int i = 0; i < kPerThread; ++i) {
                histogram.record(0.001 + static_cast<f64>((i + t) % 50) * 0.001);
            }
        });
    }
    for (auto& thread : threads) thread.join();

    CHECK_EQ(histogram.count(), static_cast<u64>(kThreads) * kPerThread);

    u64 bucket_total = 0;
    for (u32 i = 0; i < Histogram::kBucketCount; ++i) bucket_total += histogram.bucket_count_at(i);
    CHECK_EQ(bucket_total, histogram.count());
}

TEST_CASE(prometheus_exposition_is_well_formed) {
    MetricsRegistry& registry = MetricsRegistry::instance();
    registry.set_label("shard", "world-a");

    Counter* packets = registry.counter("morton_packets_total", "packets sent");
    Gauge* players = registry.gauge("morton_players", "connected players");
    Histogram* tick = registry.histogram("morton_tick_seconds", "tick duration");

    packets->add(7);
    players->set(42.5);
    for (int i = 0; i < 500; ++i) tick->record(0.004 + static_cast<f64>(i % 10) * 0.001);

    std::string text = registry.expose();

    CHECK(text.find("# TYPE morton_packets_total counter") != std::string::npos);
    CHECK(text.find("morton_packets_total{shard=\"world-a\"} 7") != std::string::npos);
    CHECK(text.find("# HELP morton_players connected players") != std::string::npos);
    CHECK(text.find("morton_players{shard=\"world-a\"} 42.5000") != std::string::npos);
    CHECK(text.find("morton_tick_seconds_count{shard=\"world-a\"} 500") != std::string::npos);
    CHECK(text.find("morton_tick_seconds_bucket{shard=\"world-a\",le=\"+Inf\"} 500") !=
          std::string::npos);
    CHECK(text.find("quantile=\"0.99\"") != std::string::npos);

    u64 previous = 0;
    std::size_t cursor = 0;
    int bucket_lines = 0;
    while (true) {
        std::size_t line_start = text.find("morton_tick_seconds_bucket", cursor);
        if (line_start == std::string::npos) break;
        std::size_t line_end = text.find('\n', line_start);
        std::string line = text.substr(line_start, line_end - line_start);
        u64 value = std::strtoull(line.c_str() + line.rfind(' ') + 1, nullptr, 10);
        CHECK(value >= previous);
        previous = value;
        ++bucket_lines;
        cursor = line_end;
    }
    CHECK(bucket_lines > 1);
    CHECK_EQ(previous, 500u);
}

TEST_CASE(http_server_routes_scrapes_and_api_calls) {
    MetricsRegistry::instance().counter("morton_scrape_probe_total")->add(3);

    HttpServer server;
    CHECK(server.start(loopback(0)));
    Address bound = server.local_address();
    CHECK(bound.port != 0);

    server.route("GET", "/metrics", [](const HttpRequest&) {
        return HttpResponse::text(MetricsRegistry::instance().expose());
    });
    server.route("GET", "/session", [](const HttpRequest& request) {
        return HttpResponse::json("{\"player\":\"" + json_escape(request.query_value("player")) +
                                  "\",\"region\":\"" + json_escape(request.query_value("region", "none")) +
                                  "\"}");
    });
    server.route("POST", "/heartbeat", [](const HttpRequest& request) {
        return HttpResponse::text("len=" + std::to_string(request.body.size()) + " ct=" +
                                  std::to_string(request.headers.count("content-length")));
    });

    std::string metrics = http_get(bound, "/metrics");
    CHECK(metrics.find("HTTP/1.1 200 OK") == 0);
    CHECK(response_body(metrics).find("morton_scrape_probe_total") != std::string::npos);

    std::string session = http_get(bound, "/session?player=ada%20l%2Bvelace&region=r1");
    CHECK(session.find("Content-Type: application/json") != std::string::npos);
    CHECK(response_body(session) == "{\"player\":\"ada l+velace\",\"region\":\"r1\"}");

    std::string defaulted = http_get(bound, "/session?player=x");
    CHECK(response_body(defaulted) == "{\"player\":\"x\",\"region\":\"none\"}");

    std::string posted = http_get(bound, "/heartbeat", "POST", "{\"shard\":\"world-a\"}");
    CHECK(response_body(posted) == "len=19 ct=1");

    std::string missing = http_get(bound, "/nope");
    CHECK(missing.find("HTTP/1.1 404 Not Found") == 0);

    CHECK_EQ(server.requests_served(), 5u);
    server.stop();
    CHECK(!server.running());
}

TEST_CASE(http_server_survives_malformed_and_truncated_requests) {
    HttpServer server;
    CHECK(server.start(loopback(0)));
    Address bound = server.local_address();
    server.route("GET", "/ping", [](const HttpRequest&) { return HttpResponse::text("pong"); });

    auto raw_send = [&](const std::string& request, bool wait_for_reply) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in sa = bound.to_sockaddr();
        if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) {
            ::close(fd);
            return std::string();
        }
        ::send(fd, request.data(), request.size(), 0);
        std::string response;
        if (wait_for_reply) {
            char chunk[2048];
            while (true) {
                ssize_t got = ::recv(fd, chunk, sizeof(chunk), 0);
                if (got <= 0) break;
                response.append(chunk, static_cast<std::size_t>(got));
            }
        }
        ::close(fd);
        return response;
    };

    raw_send("GET /ping HTTP/1.1\r\nHost: x\r\n", false);
    raw_send("", false);
    raw_send("garbage without any structure at all\r\n\r\n", true);
    raw_send("GET /ping HTTP/1.1\r\nbroken-header-no-colon\r\n\r\n", true);

    std::string ok = http_get(bound, "/ping");
    CHECK(response_body(ok) == "pong");

    server.stop();
}

TEST_MAIN()
