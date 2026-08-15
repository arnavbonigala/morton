#include <random>
#include <set>
#include <string>
#include <vector>

#include "net/reliability.h"
#include "net/sequence_buffer.h"
#include "net/udp_socket.h"
#include "tests/check.h"

using namespace morton;

namespace {

struct Entry {
    u32 value = 0;
};

/// Pumps packets between two endpoints with a configurable drop rate.
struct Link {
    ReliableEndpoint a;
    ReliableEndpoint b;
    std::mt19937 rng{7};
    f32 drop_rate = 0.f;
    u64 clock_us = 0;
    u64 rtt_us = 30000;

    std::vector<ReliableMessage> deliver(ReliableEndpoint& from, ReliableEndpoint& to,
                                         const std::string& unreliable) {
        u8 packet[kMaxDatagramSize];
        u32 size = from.write_packet(packet, sizeof(packet),
                                     reinterpret_cast<const u8*>(unreliable.data()),
                                     static_cast<u32>(unreliable.size()), clock_us);
        std::vector<ReliableMessage> received;
        if (size == 0) return received;

        bool dropped = std::uniform_real_distribution<f32>(0.f, 1.f)(rng) < drop_rate;
        if (dropped) return received;

        PayloadView payload;
        to.read_packet(packet, size, clock_us + rtt_us / 2, &payload, &received);
        return received;
    }
};

}  // namespace

TEST_CASE(sequence_buffer_clears_slots_skipped_by_a_jump) {
    SequenceBuffer<Entry, 32> buffer;
    buffer.insert(5)->value = 55;
    CHECK(buffer.exists(5));

    buffer.insert(40);
    CHECK(!buffer.exists(5));
    CHECK(buffer.exists(40));

    buffer.insert(1000);
    for (u16 s = 0; s < 900; ++s) CHECK(!buffer.exists(s));
    CHECK(buffer.exists(1000));
}

TEST_CASE(sequence_buffer_rejects_entries_older_than_the_window) {
    SequenceBuffer<Entry, 32> buffer;
    buffer.insert(100);
    CHECK(buffer.insert(99) != nullptr);
    CHECK(buffer.insert(60) == nullptr);
    CHECK(buffer.find(100) != nullptr);
}

TEST_CASE(sequence_buffer_handles_wraparound) {
    SequenceBuffer<Entry, 64> buffer;
    for (u32 i = 0; i < 200; ++i) {
        u16 sequence = static_cast<u16>(65500 + i);
        Entry* entry = buffer.insert(sequence);
        CHECK(entry != nullptr);
        entry->value = i;
    }
    Entry* recent = buffer.find(static_cast<u16>(65500 + 199));
    CHECK(recent != nullptr);
    CHECK_EQ(recent->value, 199u);
    CHECK(!buffer.exists(65500));
}

TEST_CASE(reliable_messages_arrive_once_and_in_order_under_heavy_loss) {
    Link link;
    link.drop_rate = 0.4f;

    const int kMessageCount = 200;
    for (int i = 0; i < kMessageCount; ++i) {
        std::string body = "msg-" + std::to_string(i);
        link.a.queue_reliable(static_cast<u8>(Channel::kEvent),
                              reinterpret_cast<const u8*>(body.data()),
                              static_cast<u32>(body.size()));
    }

    std::vector<std::string> delivered;
    for (int tick = 0; tick < 4000 && delivered.size() < kMessageCount; ++tick) {
        link.clock_us += 33333;
        for (const ReliableMessage& message : link.deliver(link.a, link.b, "")) {
            delivered.push_back(std::string(message.payload.begin(), message.payload.end()));
        }
        link.deliver(link.b, link.a, "");
        link.a.update(link.clock_us);
        link.b.update(link.clock_us);
    }

    CHECK_EQ(delivered.size(), static_cast<std::size_t>(kMessageCount));
    for (int i = 0; i < kMessageCount && i < static_cast<int>(delivered.size()); ++i) {
        CHECK(delivered[i] == "msg-" + std::to_string(i));
    }
    CHECK(!link.a.has_pending_reliable());
}

TEST_CASE(duplicate_packets_do_not_duplicate_reliable_messages) {
    ReliableEndpoint sender;
    ReliableEndpoint receiver;

    const std::string body = "once";
    sender.queue_reliable(3, reinterpret_cast<const u8*>(body.data()),
                          static_cast<u32>(body.size()));

    u8 packet[kMaxDatagramSize];
    u32 size = sender.write_packet(packet, sizeof(packet), nullptr, 0, 1000);
    CHECK(size > 0);

    std::vector<ReliableMessage> first;
    PayloadView view;
    CHECK(receiver.read_packet(packet, size, 2000, &view, &first));
    CHECK_EQ(first.size(), std::size_t{1});

    std::vector<ReliableMessage> second;
    CHECK(!receiver.read_packet(packet, size, 2100, &view, &second));
    CHECK_EQ(second.size(), std::size_t{0});
}

TEST_CASE(unreliable_payload_survives_reliable_traffic_in_the_same_packet) {
    ReliableEndpoint sender;
    ReliableEndpoint receiver;

    const std::string event = "migrate";
    sender.queue_reliable(static_cast<u8>(Channel::kEvent),
                          reinterpret_cast<const u8*>(event.data()),
                          static_cast<u32>(event.size()));

    std::string snapshot(600, '\x5a');
    u8 packet[kMaxDatagramSize];
    u32 size = sender.write_packet(packet, sizeof(packet),
                                   reinterpret_cast<const u8*>(snapshot.data()),
                                   static_cast<u32>(snapshot.size()), 500);
    CHECK(size > 0);

    PayloadView view;
    std::vector<ReliableMessage> messages;
    CHECK(receiver.read_packet(packet, size, 600, &view, &messages));
    CHECK_EQ(messages.size(), std::size_t{1});
    CHECK_EQ(view.size, static_cast<u32>(snapshot.size()));
    CHECK(std::string(view.data, view.data + view.size) == snapshot);
}

TEST_CASE(loss_estimate_tracks_the_real_drop_rate) {
    Link link;
    link.drop_rate = 0.25f;

    for (int tick = 0; tick < 3000; ++tick) {
        link.clock_us += 33333;
        link.deliver(link.a, link.b, "x");
        link.deliver(link.b, link.a, "x");
        link.a.update(link.clock_us);
        link.b.update(link.clock_us);
    }

    CHECK_NEAR(link.a.stats().sent_loss_percent, 25.0, 9.0);
    CHECK_NEAR(link.b.stats().recv_loss_percent, 25.0, 9.0);
    CHECK(link.a.stats().packets_acked > 1000);
}

TEST_CASE(rtt_estimate_converges_on_the_link_delay) {
    Link link;
    link.rtt_us = 80000;

    for (int tick = 0; tick < 400; ++tick) {
        link.clock_us += 33333;
        link.deliver(link.a, link.b, "x");
        link.deliver(link.b, link.a, "x");
        link.a.update(link.clock_us);
        link.b.update(link.clock_us);
    }

    CHECK_NEAR(link.a.stats().rtt_ms, 40.0, 15.0);
    CHECK(link.a.stats().rtt_ms > 0.f);
}

TEST_CASE(malformed_packets_are_rejected_without_crashing) {
    ReliableEndpoint endpoint;
    std::mt19937 rng(31337);

    for (int i = 0; i < 5000; ++i) {
        u8 junk[128];
        u32 size = 1 + (rng() % sizeof(junk));
        for (u32 b = 0; b < size; ++b) junk[b] = static_cast<u8>(rng());

        PayloadView view;
        std::vector<ReliableMessage> messages;
        if (endpoint.read_packet(junk, size, i * 1000, &view, &messages)) {
            CHECK(view.size <= size);
            for (const ReliableMessage& message : messages) {
                CHECK(message.payload.size() <= kMaxReliableMessageSize);
            }
        }
    }
}

TEST_MAIN()
