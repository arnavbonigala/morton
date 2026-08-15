#pragma once
#include <deque>
#include <functional>
#include <map>
#include <vector>

#include "core/types.h"
#include "net/protocol.h"
#include "net/sequence_buffer.h"

namespace morton {

struct ReliableMessage {
    u16 id = 0;
    u8 channel = 0;
    u32 sends = 0;
    std::vector<u8> payload;
};

struct PayloadView {
    const u8* data = nullptr;
    u32 size = 0;
};

struct NetworkStats {
    f32 rtt_ms = 0.f;
    f32 rtt_jitter_ms = 0.f;
    f32 rtt_min_ms = 0.f;
    f32 sent_loss_percent = 0.f;
    f32 recv_loss_percent = 0.f;
    f32 sent_kbps = 0.f;
    f32 recv_kbps = 0.f;
    u64 packets_sent = 0;
    u64 packets_received = 0;
    u64 packets_acked = 0;
    u64 packets_lost = 0;
    u64 reliable_retransmits = 0;
};

/// Sequence/ack reliability over an unreliable datagram stream.
///
/// Every outgoing packet carries a sequence, the newest sequence received from
/// the peer, and a 32-bit history of the 32 before it, so a single received
/// packet acknowledges up to 33. Reliable messages piggyback on that ack stream:
/// they ride in normal packets and are retransmitted until a packet carrying
/// them is acked, so there is no separate retransmit timer per message.
class ReliableEndpoint {
public:
    ReliableEndpoint();

    void reset();

    /// Enqueues a message for guaranteed, in-order delivery on `channel`.
    void queue_reliable(u8 channel, const u8* data, u32 size);

    /// Serializes sequence, acks, pending reliable messages and then `unreliable`
    /// into `out`. The caller passes a cursor just past its own outer header.
    /// Returns bytes written, or 0 if the packet could not be built.
    u32 write_packet(u8* out, u32 capacity, const u8* unreliable, u32 unreliable_size,
                     u64 now_us);

    /// Parses a packet body written by write_packet. Received reliable messages are
    /// appended to `out_messages` in order, already deduplicated. Returns false if
    /// the packet is malformed or a duplicate.
    bool read_packet(const u8* data, u32 size, u64 now_us, PayloadView* out_unreliable,
                     std::vector<ReliableMessage>* out_messages);

    /// Recomputes rate estimates; call once per tick.
    void update(u64 now_us);

    const NetworkStats& stats() const { return stats_; }
    u16 next_sequence() const { return sequence_; }
    bool has_pending_reliable() const { return !unacked_.empty(); }
    u32 pending_reliable_count() const { return static_cast<u32>(unacked_.size()); }

private:
    struct SentPacket {
        u64 send_time_us = 0;
        u32 size = 0;
        bool acked = false;
        u32 message_count = 0;
        u16 message_ids[kMaxReliablePerPacket] = {};
    };

    struct ReceivedPacket {
        u64 recv_time_us = 0;
        u32 size = 0;
    };

    void process_acks(u16 ack, u32 ack_bits, u64 now_us);
    void on_packet_acked(u16 sequence, u64 now_us);
    void deliver_in_order(std::vector<ReliableMessage>* out);
    void blend(f32* target, f32 sample) const;

    u16 sequence_ = 0;
    u16 next_message_id_ = 0;
    u16 expected_message_id_ = 0;

    SequenceBuffer<SentPacket, kAckWindowSize> sent_;
    SequenceBuffer<ReceivedPacket, kAckWindowSize> received_;

    std::deque<ReliableMessage> unacked_;
    std::map<u16, ReliableMessage> reorder_buffer_;

    NetworkStats stats_;
    f32 rtt_ewma_ms_ = 0.f;
    f32 rtt_min_ms_ = 0.f;
    f32 jitter_ewma_ms_ = 0.f;
    bool rtt_initialized_ = false;

    u64 rate_window_start_us_ = 0;
    u64 bytes_sent_window_ = 0;
    u64 bytes_recv_window_ = 0;
    u64 packets_recv_window_ = 0;
    u64 packets_expected_window_ = 0;
    u64 packets_acked_window_ = 0;
    u64 packets_lost_window_ = 0;

    u16 highest_received_ = 0;
    bool has_received_ = false;

    bool rates_initialized_ = false;
    u16 loss_cursor_ = 0;
    bool has_loss_cursor_ = false;
};

}  // namespace morton
