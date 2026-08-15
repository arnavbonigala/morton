#include <string>
#include <vector>

#include "check.h"
#include "core/time.h"
#include "net/websocket.h"
#include "ws_client.h"

using namespace morton;
using morton_test::WsClient;

namespace {

Address loopback() { return Address(0x7f000001u, 0); }

bool wait_for_clients(const WebSocketServer& server, u32 expected, u32 attempts = 200) {
    for (u32 i = 0; i < attempts; ++i) {
        if (server.client_count() == expected) return true;
        sleep_us(10000);
    }
    return server.client_count() == expected;
}

}  // namespace

/// RFC 6455 section 1.3 fixes this key/accept pair; get it wrong and no browser
/// will complete the upgrade.
TEST_CASE(the_handshake_accept_key_matches_the_rfc_vector) {
    CHECK(websocket_accept_key("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");

    const u8 empty[1] = {0};
    CHECK(base64_encode(empty, 0).empty());
    CHECK(base64_encode(reinterpret_cast<const u8*>("M"), 1) == "TQ==");
    CHECK(base64_encode(reinterpret_cast<const u8*>("Ma"), 2) == "TWE=");
    CHECK(base64_encode(reinterpret_cast<const u8*>("Man"), 3) == "TWFu");
}

TEST_CASE(frames_use_the_length_encoding_the_payload_requires) {
    std::string tiny = encode_text_frame("hi");
    CHECK_EQ(static_cast<u32>(static_cast<u8>(tiny[0])), 0x81u);
    CHECK_EQ(static_cast<u32>(static_cast<u8>(tiny[1])), 2u);

    std::string medium = encode_text_frame(std::string(200, 'x'));
    CHECK_EQ(static_cast<u32>(static_cast<u8>(medium[1])), 126u);
    CHECK_EQ(static_cast<u32>((static_cast<u8>(medium[2]) << 8) | static_cast<u8>(medium[3])), 200u);
    CHECK_EQ(static_cast<u32>(medium.size()), 204u);

    std::string large = encode_text_frame(std::string(70000, 'y'));
    CHECK_EQ(static_cast<u32>(static_cast<u8>(large[1])), 127u);
    CHECK_EQ(static_cast<u32>(large.size()), 70010u);
}

TEST_CASE(a_real_client_receives_published_frames_of_every_size) {
    WebSocketConfig config;
    config.bind = loopback();

    WebSocketServer server;
    CHECK(server.start(config));

    WsClient client;
    CHECK(client.connect(server.local_address()));
    CHECK(client.handshake_response().find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);
    CHECK(wait_for_clients(server, 1));

    std::string small = "{\"tick\":1}";
    std::string medium(4000, 'm');
    std::string large(80000, 'l');

    server.publish(small);
    std::string received;
    CHECK(client.receive(&received));
    CHECK(received == small);

    server.publish(medium);
    CHECK(client.receive(&received));
    CHECK(received == medium);

    server.publish(large);
    CHECK(client.receive(&received));
    CHECK(received == large);
    CHECK(server.frames_sent() >= 3u);

    server.stop();
}

TEST_CASE(a_departing_viewer_is_reaped_and_publishing_to_nobody_is_free) {
    WebSocketConfig config;
    config.bind = loopback();
    config.max_queued_frames = 4;

    WebSocketServer server;
    CHECK(server.start(config));

    {
        WsClient client;
        CHECK(client.connect(server.local_address()));
        CHECK(wait_for_clients(server, 1));
        client.send_close();
    }
    CHECK(wait_for_clients(server, 0));

    u64 dropped_before = server.frames_dropped();
    for (u32 i = 0; i < 1000; ++i) server.publish("{\"tick\":" + std::to_string(i) + "}");
    CHECK_EQ(server.frames_dropped(), dropped_before);
    CHECK_EQ(server.frames_sent(), 0ull);

    WsClient late;
    CHECK(late.connect(server.local_address()));
    CHECK(wait_for_clients(server, 1));
    server.publish("{\"tick\":\"fresh\"}");

    std::string received;
    CHECK(late.receive(&received));
    CHECK(received == "{\"tick\":\"fresh\"}");

    server.stop();
}

/// The client deliberately never reads: a viewer that falls behind must not be
/// able to slow the simulation tick that publishes to it.
TEST_CASE(a_slow_viewer_is_bounded_and_never_stalls_the_publisher) {
    WebSocketConfig config;
    config.bind = loopback();
    config.max_queued_frames = 4;

    WebSocketServer server;
    CHECK(server.start(config));

    WsClient client;
    CHECK(client.connect(server.local_address()));
    CHECK(wait_for_clients(server, 1));

    u64 started = now_us();
    std::string payload(16000, 'z');
    for (u32 i = 0; i < 5000; ++i) server.publish(payload);
    u64 elapsed = now_us() - started;

    CHECK(elapsed < 500000ull);
    CHECK(server.frames_dropped() > 0);
    CHECK(server.running());

    server.stop();
}

TEST_MAIN()
