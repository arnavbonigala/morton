#include "net/reliability.h"

#include "core/log.h"
#include "proto/bitstream.h"

namespace morton {
namespace {

constexpr u64 kRateWindowUs = 250000;
constexpr f32 kRttSmoothing = 0.10f;
constexpr f32 kJitterSmoothing = 0.10f;
constexpr f32 kRateSmoothing = 0.10f;

/// A packet older than this many sequences with no ack is counted lost. At 30Hz
/// this is a full second, far beyond any plausible reordering delay.
constexpr u16 kLossHorizon = 32;

}  // namespace

ReliableEndpoint::ReliableEndpoint() { reset(); }

void ReliableEndpoint::reset() {
    sequence_ = 0;
    next_message_id_ = 0;
    expected_message_id_ = 0;
    sent_.reset();
    received_.reset();
    unacked_.clear();
    reorder_buffer_.clear();
    stats_ = NetworkStats{};
    rtt_ewma_ms_ = 0.f;
    rtt_min_ms_ = 0.f;
    jitter_ewma_ms_ = 0.f;
    rtt_initialized_ = false;
    rates_initialized_ = false;
    rate_window_start_us_ = 0;
    bytes_sent_window_ = 0;
    bytes_recv_window_ = 0;
    packets_recv_window_ = 0;
    packets_expected_window_ = 0;
    packets_acked_window_ = 0;
    packets_lost_window_ = 0;
    highest_received_ = 0;
    has_received_ = false;
    loss_cursor_ = 0;
    has_loss_cursor_ = false;
}

void ReliableEndpoint::queue_reliable(u8 channel, const u8* data, u32 size) {
    if (size == 0 || size > kMaxReliableMessageSize) {
        MORTON_LOG_WARN("dropping reliable message of invalid size %u", size);
        return;
    }
    ReliableMessage message;
    message.id = next_message_id_++;
    message.channel = channel;
    message.payload.assign(data, data + size);
    unacked_.push_back(std::move(message));
}

u32 ReliableEndpoint::write_packet(u8* out, u32 capacity, const u8* unreliable,
                                   u32 unreliable_size, u64 now_us) {
    BitWriter writer(out, capacity);

    u16 sequence = sequence_++;
    writer.write_u16(sequence);
    writer.write_u16(has_received_ ? highest_received_ : 0);

    u32 ack_bits = 0;
    if (has_received_) {
        for (u32 i = 0; i < 32; ++i) {
            u16 previous = static_cast<u16>(highest_received_ - (i + 1));
            if (received_.exists(previous)) ack_bits |= (1u << i);
        }
    }
    writer.write_u32(ack_bits);

    SentPacket* record = sent_.insert(sequence);
    if (record == nullptr) return 0;
    record->send_time_us = now_us;
    record->acked = false;
    record->message_count = 0;

    u32 include_count = 0;
    for (const ReliableMessage& message : unacked_) {
        if (include_count >= kMaxReliablePerPacket) break;
        u32 needed_bits = 16 + 8 + 16 + message.payload.size() * 8;
        if (writer.bits_remaining() < needed_bits + unreliable_size * 8 + 32) break;
        ++include_count;
    }

    writer.write_u8(static_cast<u8>(include_count));
    u32 written_messages = 0;
    for (ReliableMessage& message : unacked_) {
        if (written_messages >= include_count) break;
        writer.write_u16(message.id);
        writer.write_u8(message.channel);
        writer.write_u16(static_cast<u16>(message.payload.size()));
        writer.write_bytes(message.payload.data(), static_cast<u32>(message.payload.size()));
        record->message_ids[record->message_count++] = message.id;
        ++written_messages;
        if (message.sends++ > 0) ++stats_.reliable_retransmits;
    }

    writer.write_u16(static_cast<u16>(unreliable_size));
    if (unreliable_size > 0) writer.write_bytes(unreliable, unreliable_size);

    if (writer.overflowed()) {
        MORTON_LOG_WARN("packet build overflowed (%u bytes capacity)", capacity);
        return 0;
    }

    u32 size = writer.bytes_written();
    record->size = size;
    ++stats_.packets_sent;
    bytes_sent_window_ += size;
    return size;
}

bool ReliableEndpoint::read_packet(const u8* data, u32 size, u64 now_us,
                                   PayloadView* out_unreliable,
                                   std::vector<ReliableMessage>* out_messages) {
    BitReader reader(data, size);

    u16 sequence = reader.read_u16();
    u16 ack = reader.read_u16();
    u32 ack_bits = reader.read_u32();
    if (reader.overflowed()) return false;

    if (received_.exists(sequence)) return false;

    ReceivedPacket* record = received_.insert(sequence);
    if (record == nullptr) return false;
    record->recv_time_us = now_us;
    record->size = size;

    if (!has_received_) {
        highest_received_ = sequence;
        has_received_ = true;
        packets_expected_window_ += 1;
    } else if (sequence_greater(sequence, highest_received_)) {
        packets_expected_window_ += static_cast<u16>(sequence - highest_received_);
        highest_received_ = sequence;
    }

    ++stats_.packets_received;
    ++packets_recv_window_;
    bytes_recv_window_ += size;

    process_acks(ack, ack_bits, now_us);

    u32 message_count = reader.read_u8();
    if (message_count > kMaxReliablePerPacket) return false;

    for (u32 i = 0; i < message_count; ++i) {
        ReliableMessage message;
        message.id = reader.read_u16();
        message.channel = reader.read_u8();
        u16 payload_size = reader.read_u16();
        if (payload_size > kMaxReliableMessageSize || reader.overflowed()) return false;
        message.payload.resize(payload_size);
        reader.read_bytes(message.payload.data(), payload_size);
        if (reader.overflowed()) return false;

        bool already_delivered = !sequence_greater(message.id, expected_message_id_) &&
                                 message.id != expected_message_id_;
        if (already_delivered) continue;
        if (reorder_buffer_.count(message.id) != 0) continue;
        reorder_buffer_.emplace(message.id, std::move(message));
    }

    u16 unreliable_size = reader.read_u16();
    if (reader.overflowed()) return false;
    if (reader.bytes_read() + unreliable_size > size) return false;

    if (unreliable_size > 0) {
        out_unreliable->data = data + reader.bytes_read();
        out_unreliable->size = unreliable_size;
    } else {
        out_unreliable->data = nullptr;
        out_unreliable->size = 0;
    }

    deliver_in_order(out_messages);
    return true;
}

void ReliableEndpoint::deliver_in_order(std::vector<ReliableMessage>* out) {
    auto it = reorder_buffer_.find(expected_message_id_);
    while (it != reorder_buffer_.end()) {
        out->push_back(std::move(it->second));
        reorder_buffer_.erase(it);
        ++expected_message_id_;
        it = reorder_buffer_.find(expected_message_id_);
    }
}

void ReliableEndpoint::process_acks(u16 ack, u32 ack_bits, u64 now_us) {
    on_packet_acked(ack, now_us);
    for (u32 i = 0; i < 32; ++i) {
        if ((ack_bits & (1u << i)) != 0) {
            on_packet_acked(static_cast<u16>(ack - (i + 1)), now_us);
        }
    }
}

void ReliableEndpoint::on_packet_acked(u16 sequence, u64 now_us) {
    SentPacket* record = sent_.find(sequence);
    if (record == nullptr || record->acked) return;

    record->acked = true;
    ++stats_.packets_acked;
    ++packets_acked_window_;

    f32 sample_ms = static_cast<f32>(now_us - record->send_time_us) / 1000.f;
    if (!rtt_initialized_) {
        rtt_ewma_ms_ = sample_ms;
        rtt_min_ms_ = sample_ms;
        rtt_initialized_ = true;
    } else {
        jitter_ewma_ms_ +=
            (std::fabs(sample_ms - rtt_ewma_ms_) - jitter_ewma_ms_) * kJitterSmoothing;
        rtt_ewma_ms_ += (sample_ms - rtt_ewma_ms_) * kRttSmoothing;
        if (sample_ms < rtt_min_ms_) rtt_min_ms_ = sample_ms;
    }
    stats_.rtt_ms = rtt_ewma_ms_;
    stats_.rtt_jitter_ms = jitter_ewma_ms_;
    stats_.rtt_min_ms = rtt_min_ms_;

    for (u32 i = 0; i < record->message_count; ++i) {
        u16 message_id = record->message_ids[i];
        for (auto it = unacked_.begin(); it != unacked_.end(); ++it) {
            if (it->id == message_id) {
                unacked_.erase(it);
                break;
            }
        }
    }
}

void ReliableEndpoint::blend(f32* target, f32 sample) const {
    *target = rates_initialized_ ? *target + (sample - *target) * kRateSmoothing : sample;
}

void ReliableEndpoint::update(u64 now_us) {
    if (rate_window_start_us_ == 0) rate_window_start_us_ = now_us;

    u16 horizon_end = static_cast<u16>(sequence_ - kLossHorizon);
    if (!has_loss_cursor_) {
        loss_cursor_ = horizon_end;
        has_loss_cursor_ = true;
    }
    while (sequence_greater(horizon_end, loss_cursor_)) {
        const SentPacket* record = sent_.find(loss_cursor_);
        if (record != nullptr && !record->acked) {
            ++stats_.packets_lost;
            ++packets_lost_window_;
        }
        ++loss_cursor_;
    }

    u64 elapsed_us = now_us - rate_window_start_us_;
    if (elapsed_us < kRateWindowUs) return;

    f32 elapsed_s = static_cast<f32>(elapsed_us) / 1e6f;
    f32 sent_kbps = static_cast<f32>(bytes_sent_window_) * 8.f / 1000.f / elapsed_s;
    f32 recv_kbps = static_cast<f32>(bytes_recv_window_) * 8.f / 1000.f / elapsed_s;

    u64 sent_judged = packets_acked_window_ + packets_lost_window_;
    u64 missing = packets_expected_window_ > packets_recv_window_
                      ? packets_expected_window_ - packets_recv_window_
                      : 0;

    blend(&stats_.sent_kbps, sent_kbps);
    blend(&stats_.recv_kbps, recv_kbps);
    if (sent_judged > 0) {
        blend(&stats_.sent_loss_percent,
              static_cast<f32>(packets_lost_window_) * 100.f / static_cast<f32>(sent_judged));
    }
    if (packets_expected_window_ > 0) {
        blend(&stats_.recv_loss_percent,
              static_cast<f32>(missing) * 100.f / static_cast<f32>(packets_expected_window_));
    }
    rates_initialized_ = true;

    stats_.rtt_ms = rtt_ewma_ms_;
    stats_.rtt_jitter_ms = jitter_ewma_ms_;
    stats_.rtt_min_ms = rtt_min_ms_;

    rate_window_start_us_ = now_us;
    bytes_sent_window_ = 0;
    bytes_recv_window_ = 0;
    packets_recv_window_ = 0;
    packets_expected_window_ = 0;
    packets_acked_window_ = 0;
    packets_lost_window_ = 0;
}

}  // namespace morton
