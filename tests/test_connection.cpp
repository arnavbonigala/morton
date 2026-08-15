#include <string>
#include <vector>

#include "core/time.h"
#include "net/connection.h"
#include "tests/check.h"

using namespace morton;

namespace {

ConnectToken make_token(u8 seed) {
    ConnectToken token{};
    for (u32 i = 0; i < kConnectTokenBytes; ++i) token[i] = static_cast<u8>(seed + i);
    return token;
}

/// Steps a server and a set of clients until `done` or the deadline passes.
template <typename DoneFn>
void pump(ConnectionServer& server, std::vector<ClientConnection*> clients, DoneFn done,
          u64 budget_us = 3000000) {
    u64 start = now_us();
    while (now_us() - start < budget_us) {
        u64 clock = now_us();
        server.receive(clock);
        server.flush(clock);
        server.timeout_connections(clock);
        for (ClientConnection* client : clients) client->update(clock);
        if (done()) return;
        sleep_us(1000);
    }
}

}  // namespace

TEST_CASE(handshake_completes_and_carries_payloads_both_ways) {
    ServerConfig config;
    config.bind = Address(0x7f000001, 0);
    ConnectionServer server;
    CHECK(server.start(config));

    ConnectToken expected = make_token(11);
    ConnectToken seen{};
    server.set_admit_handler([&](const Address&, const ConnectToken& token) {
        seen = token;
        AdmitDecision decision;
        decision.admit = true;
        decision.client_id = 4242;
        return decision;
    });

    std::vector<std::string> server_payloads;
    server.set_payload_handler([&](Connection&, const u8* data, u32 size) {
        server_payloads.push_back(std::string(reinterpret_cast<const char*>(data), size));
    });

    ClientConfig client_config;
    client_config.server = server.local_address();
    client_config.token = expected;
    ClientConnection client;
    CHECK(client.start(client_config));

    pump(server, {&client}, [&] { return client.connected(); });
    CHECK(client.connected());
    CHECK_EQ(client.client_id(), 4242u);
    CHECK_EQ(server.connection_count(), 1u);
    CHECK(seen == expected);

    const std::string input = "input-frame";
    client.send_payload(reinterpret_cast<const u8*>(input.data()),
                        static_cast<u32>(input.size()), now_us());
    pump(server, {&client}, [&] { return !server_payloads.empty(); });
    CHECK_EQ(server_payloads.size(), std::size_t{1});
    CHECK(server_payloads[0] == input);

    std::vector<std::string> client_payloads;
    client.set_payload_handler([&](const u8* data, u32 size) {
        client_payloads.push_back(std::string(reinterpret_cast<const char*>(data), size));
    });

    const std::string snapshot = "snapshot-frame";
    Connection* connection = server.find(4242);
    CHECK(connection != nullptr);
    server.send_payload(*connection, reinterpret_cast<const u8*>(snapshot.data()),
                        static_cast<u32>(snapshot.size()), now_us());
    pump(server, {&client}, [&] { return !client_payloads.empty(); });
    CHECK_EQ(client_payloads.size(), std::size_t{1});
    CHECK(client_payloads[0] == snapshot);

    server.stop();
}

TEST_CASE(rejected_token_denies_the_client) {
    ServerConfig config;
    config.bind = Address(0x7f000001, 0);
    ConnectionServer server;
    CHECK(server.start(config));

    server.set_admit_handler([](const Address&, const ConnectToken&) {
        AdmitDecision decision;
        decision.admit = false;
        decision.reason = DenyReason::kBadToken;
        return decision;
    });

    ClientConfig client_config;
    client_config.server = server.local_address();
    client_config.token = make_token(3);
    ClientConnection client;
    CHECK(client.start(client_config));

    pump(server, {&client}, [&] { return client.state() == ClientState::kDenied; });
    CHECK(client.state() == ClientState::kDenied);
    CHECK(client.deny_reason() == DenyReason::kBadToken);
    CHECK_EQ(server.connection_count(), 0u);

    server.stop();
}

TEST_CASE(server_full_is_reported_rather_than_silently_dropped) {
    ServerConfig config;
    config.bind = Address(0x7f000001, 0);
    config.max_connections = 1;
    ConnectionServer server;
    CHECK(server.start(config));

    ClientConfig base;
    base.server = server.local_address();
    base.token = make_token(5);

    ClientConnection first;
    CHECK(first.start(base));
    pump(server, {&first}, [&] { return first.connected(); });
    CHECK(first.connected());

    ClientConnection second;
    CHECK(second.start(base));
    pump(server, {&first, &second}, [&] { return second.state() == ClientState::kDenied; });
    CHECK(second.state() == ClientState::kDenied);
    CHECK(second.deny_reason() == DenyReason::kServerFull);

    server.stop();
}

TEST_CASE(silent_client_is_timed_out_by_the_server) {
    ServerConfig config;
    config.bind = Address(0x7f000001, 0);
    config.timeout_us = 200000;
    ConnectionServer server;
    CHECK(server.start(config));

    bool disconnected = false;
    server.set_disconnect_handler([&](Connection&, const char*) { disconnected = true; });

    ClientConfig client_config;
    client_config.server = server.local_address();
    client_config.token = make_token(9);
    ClientConnection client;
    CHECK(client.start(client_config));

    pump(server, {&client}, [&] { return client.connected(); });
    CHECK(client.connected());

    u64 start = now_us();
    while (now_us() - start < 1500000 && !disconnected) {
        server.receive(now_us());
        server.flush(now_us());
        server.timeout_connections(now_us());
        sleep_us(2000);
    }

    CHECK(disconnected);
    CHECK_EQ(server.connection_count(), 0u);
    server.stop();
}

TEST_CASE(reliable_events_reach_the_client_in_order) {
    ServerConfig config;
    config.bind = Address(0x7f000001, 0);
    ConnectionServer server;
    CHECK(server.start(config));

    ClientConfig client_config;
    client_config.server = server.local_address();
    client_config.token = make_token(1);
    ClientConnection client;

    std::vector<std::string> events;
    client.set_message_handler([&](const ReliableMessage& message) {
        events.push_back(std::string(message.payload.begin(), message.payload.end()));
    });

    CHECK(client.start(client_config));
    pump(server, {&client}, [&] { return client.connected(); });
    CHECK(client.connected());

    Connection* connection = nullptr;
    server.for_each_connection([&](Connection& c) { connection = &c; });
    CHECK(connection != nullptr);

    for (int i = 0; i < 20; ++i) {
        std::string body = "event-" + std::to_string(i);
        connection->reliability.queue_reliable(static_cast<u8>(Channel::kEvent),
                                               reinterpret_cast<const u8*>(body.data()),
                                               static_cast<u32>(body.size()));
    }

    pump(server, {&client}, [&] { return events.size() >= 20; });
    CHECK_EQ(events.size(), std::size_t{20});
    for (int i = 0; i < 20 && i < static_cast<int>(events.size()); ++i) {
        CHECK(events[i] == "event-" + std::to_string(i));
    }

    server.stop();
}

TEST_CASE(client_survives_redirect_to_a_second_server) {
    ServerConfig config;
    config.bind = Address(0x7f000001, 0);

    ConnectionServer first;
    ConnectionServer second;
    CHECK(first.start(config));
    CHECK(second.start(config));

    ClientConfig client_config;
    client_config.server = first.local_address();
    client_config.token = make_token(21);
    ClientConnection client;
    CHECK(client.start(client_config));

    pump(first, {&client}, [&] { return client.connected(); });
    CHECK(client.connected());
    CHECK_EQ(first.connection_count(), 1u);

    client.redirect(second.local_address(), make_token(77), now_us());

    u64 start = now_us();
    while (now_us() - start < 3000000 && !client.connected()) {
        u64 clock = now_us();
        first.receive(clock);
        second.receive(clock);
        second.flush(clock);
        client.update(clock);
        sleep_us(1000);
    }

    CHECK(client.connected());
    CHECK_EQ(second.connection_count(), 1u);

    first.stop();
    second.stop();
}

TEST_MAIN()
