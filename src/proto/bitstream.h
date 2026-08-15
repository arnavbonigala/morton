#pragma once
#include <cstring>
#include <string>
#include <vector>

#include "core/types.h"

namespace morton {

/// LSB-first bit writer over a caller-owned buffer. Never writes past capacity;
/// overflow latches an error flag instead so callers can bail on a full packet.
class BitWriter {
public:
    BitWriter(u8* buffer, u32 capacity) : buffer_(buffer), capacity_bits_(capacity * 8) {}

    void write_bits(u32 value, u32 bits) {
        if (bits == 0) return;
        if (bit_pos_ + bits > capacity_bits_) { overflow_ = true; return; }
        if (bits < 32) value &= (1u << bits) - 1u;

        accumulator_ |= static_cast<u64>(value) << pending_bits_;
        pending_bits_ += bits;
        bit_pos_ += bits;
        while (pending_bits_ >= 8) {
            buffer_[flushed_bytes_++] = static_cast<u8>(accumulator_);
            accumulator_ >>= 8;
            pending_bits_ -= 8;
        }
    }

    void write_bool(bool value) { write_bits(value ? 1u : 0u, 1); }
    void write_u8(u8 value) { write_bits(value, 8); }
    void write_u16(u16 value) { write_bits(value, 16); }
    void write_u32(u32 value) { write_bits(value, 32); }

    void write_u64(u64 value) {
        write_bits(static_cast<u32>(value & 0xffffffffu), 32);
        write_bits(static_cast<u32>(value >> 32), 32);
    }

    void write_float(f32 value) {
        u32 bits;
        std::memcpy(&bits, &value, sizeof(bits));
        write_bits(bits, 32);
    }

    /// Writes an integer known to lie in [min, max] using only the needed bits.
    void write_ranged(i32 value, i32 min, i32 max) {
        u32 bits = bits_required(min, max);
        if (value < min) value = min;
        if (value > max) value = max;
        write_bits(static_cast<u32>(value - min), bits);
    }

    /// Variable-length integer: 7 payload bits per byte, high bit continues.
    void write_varint(u32 value) {
        while (value >= 0x80) {
            write_bits((value & 0x7f) | 0x80, 8);
            value >>= 7;
        }
        write_bits(value & 0x7f, 8);
    }

    void write_string(const std::string& value) {
        u32 len = static_cast<u32>(value.size() > 1023 ? 1023 : value.size());
        write_varint(len);
        for (u32 i = 0; i < len; ++i) write_u8(static_cast<u8>(value[i]));
    }

    void write_bytes(const u8* data, u32 count) {
        if (pending_bits_ != 0) {
            for (u32 i = 0; i < count; ++i) write_u8(data[i]);
            return;
        }
        if (static_cast<u64>(bit_pos_) + static_cast<u64>(count) * 8 > capacity_bits_) {
            overflow_ = true;
            return;
        }
        std::memcpy(buffer_ + flushed_bytes_, data, count);
        flushed_bytes_ += count;
        bit_pos_ += count * 8;
    }

    void align() {
        u32 rem = bit_pos_ & 7;
        if (rem != 0) write_bits(0, 8 - rem);
    }

    u32 bits_written() const { return bit_pos_; }

    /// Materialises the trailing partial byte, so the buffer is complete as far
    /// as the returned length. Safe to call repeatedly and to keep writing after.
    u32 bytes_written() {
        if (pending_bits_ > 0) buffer_[flushed_bytes_] = static_cast<u8>(accumulator_);
        return flushed_bytes_ + (pending_bits_ > 0 ? 1u : 0u);
    }

    u32 bits_remaining() const { return capacity_bits_ - bit_pos_; }
    bool overflowed() const { return overflow_; }

    static u32 bits_required(i32 min, i32 max) {
        u32 range = static_cast<u32>(static_cast<i64>(max) - static_cast<i64>(min));
        u32 bits = 0;
        while (range > 0) { ++bits; range >>= 1; }
        return bits == 0 ? 1 : bits;
    }

private:
    u8* buffer_;
    u32 capacity_bits_;
    u32 bit_pos_ = 0;
    u64 accumulator_ = 0;
    u32 pending_bits_ = 0;
    u32 flushed_bytes_ = 0;
    bool overflow_ = false;
};

/// Mirror of BitWriter. Reads past the end return zero and latch the error flag.
class BitReader {
public:
    BitReader(const u8* buffer, u32 size) : buffer_(buffer), capacity_bits_(size * 8) {}

    u32 read_bits(u32 bits) {
        if (bits == 0) return 0;
        if (bit_pos_ + bits > capacity_bits_) { overflow_ = true; return 0; }
        u32 result = 0;
        u32 written = 0;
        u32 remaining = bits;
        while (remaining > 0) {
            u32 byte_index = bit_pos_ >> 3;
            u32 bit_offset = bit_pos_ & 7;
            u32 avail_in_byte = 8 - bit_offset;
            u32 take = remaining < avail_in_byte ? remaining : avail_in_byte;
            u32 chunk = (buffer_[byte_index] >> bit_offset) & ((1u << take) - 1u);
            result |= chunk << written;
            written += take;
            remaining -= take;
            bit_pos_ += take;
        }
        return result;
    }

    bool read_bool() { return read_bits(1) != 0; }
    u8 read_u8() { return static_cast<u8>(read_bits(8)); }
    u16 read_u16() { return static_cast<u16>(read_bits(16)); }
    u32 read_u32() { return read_bits(32); }

    u64 read_u64() {
        u64 low = read_bits(32);
        u64 high = read_bits(32);
        return low | (high << 32);
    }

    f32 read_float() {
        u32 bits = read_bits(32);
        f32 value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    i32 read_ranged(i32 min, i32 max) {
        u32 bits = BitWriter::bits_required(min, max);
        return static_cast<i32>(read_bits(bits)) + min;
    }

    u32 read_varint() {
        u32 result = 0;
        u32 shift = 0;
        for (int i = 0; i < 5; ++i) {
            u32 byte = read_bits(8);
            result |= (byte & 0x7f) << shift;
            if ((byte & 0x80) == 0) break;
            shift += 7;
        }
        return result;
    }

    std::string read_string() {
        u32 len = read_varint();
        if (len > 1023) { overflow_ = true; return {}; }
        std::string out;
        out.reserve(len);
        for (u32 i = 0; i < len; ++i) out.push_back(static_cast<char>(read_u8()));
        return out;
    }

    void read_bytes(u8* out, u32 count) {
        if ((bit_pos_ & 7) != 0) {
            for (u32 i = 0; i < count; ++i) out[i] = read_u8();
            return;
        }
        if (static_cast<u64>(bit_pos_) + static_cast<u64>(count) * 8 > capacity_bits_) {
            overflow_ = true;
            return;
        }
        std::memcpy(out, buffer_ + (bit_pos_ >> 3), count);
        bit_pos_ += count * 8;
    }

    void align() {
        u32 rem = bit_pos_ & 7;
        if (rem != 0) read_bits(8 - rem);
    }

    u32 bits_read() const { return bit_pos_; }
    u32 bytes_read() const { return (bit_pos_ + 7) >> 3; }
    u32 bits_remaining() const { return capacity_bits_ - bit_pos_; }
    bool overflowed() const { return overflow_; }

private:
    const u8* buffer_;
    u32 capacity_bits_;
    u32 bit_pos_ = 0;
    bool overflow_ = false;
};

/// Fixed-point quantization used for all replicated positions and velocities.
struct Quantizer {
    f32 min;
    f32 max;
    u32 bits;

    u32 encode(f32 value) const {
        f32 clamped = clampf(value, min, max);
        f32 normalized = (clamped - min) / (max - min);
        u32 levels = (bits >= 32) ? 0xffffffffu : ((1u << bits) - 1u);
        return static_cast<u32>(normalized * static_cast<f32>(levels) + 0.5f);
    }

    f32 decode(u32 quantized) const {
        u32 levels = (bits >= 32) ? 0xffffffffu : ((1u << bits) - 1u);
        f32 normalized = static_cast<f32>(quantized) / static_cast<f32>(levels);
        return min + normalized * (max - min);
    }

    /// Round-trips a value through quantization so server and client agree bit-exactly.
    f32 snap(f32 value) const { return decode(encode(value)); }
};

}  // namespace morton
