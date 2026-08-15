#include <cstring>

#include "net/udp_socket.h"
#include "tests/check.h"

using namespace morton;

namespace {

UdpSocket bound() {
    UdpSocket socket;
    CHECK(socket.open(Address(0x7F000001u, 0), 4 * 1024 * 1024));
    return socket;
}

}  // namespace

TEST_CASE(a_batch_larger_than_the_batch_window_arrives_whole_and_in_order) {
    UdpSocket sender = bound();
    UdpSocket receiver = bound();

    const u32 total = 300;
    for (u32 i = 0; i < total; ++i) {
        u8 payload[64];
        std::memset(payload, 0, sizeof(payload));
        std::memcpy(payload, &i, sizeof(i));
        CHECK(sender.send_batched(receiver.local_address(), payload, sizeof(payload)));
    }
    sender.flush_sends();

    u32 received = 0;
    Address from;
    u8 buffer[128];
    while (received < total && receiver.wait_readable(500000)) {
        int size = receiver.receive(&from, buffer, sizeof(buffer));
        if (size <= 0) continue;
        CHECK_EQ(size, 64);
        u32 sequence = 0;
        std::memcpy(&sequence, buffer, sizeof(sequence));
        CHECK_EQ(sequence, received);
        ++received;
    }

    CHECK_EQ(received, total);
    CHECK_EQ(sender.packets_sent(), static_cast<u64>(total));
    CHECK_EQ(receiver.packets_received(), static_cast<u64>(total));
}

TEST_CASE(closing_a_socket_flushes_what_was_still_pending) {
    UdpSocket receiver = bound();
    Address destination = receiver.local_address();

    {
        UdpSocket sender = bound();
        const u8 payload[] = {7, 7, 7, 7};
        CHECK(sender.send_batched(destination, payload, sizeof(payload)));
        CHECK_EQ(sender.packets_sent(), 0u);
    }

    CHECK(receiver.wait_readable(500000));
    Address from;
    u8 buffer[16];
    CHECK_EQ(receiver.receive(&from, buffer, sizeof(buffer)), 4);
    CHECK_EQ(buffer[0], 7);
}

TEST_CASE(an_oversized_datagram_is_refused_rather_than_truncated) {
    UdpSocket sender = bound();
    UdpSocket receiver = bound();

    std::vector<u8> payload(kMaxDatagramSize + 1, 0xAB);
    CHECK(!sender.send_batched(receiver.local_address(), payload.data(),
                               static_cast<u32>(payload.size())));
    sender.flush_sends();
    CHECK_EQ(sender.packets_sent(), 0u);
}

TEST_MAIN()
