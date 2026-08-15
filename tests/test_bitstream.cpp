#include <random>
#include <vector>

#include "proto/bitstream.h"
#include "tests/check.h"

using namespace morton;

TEST_CASE(mixed_width_roundtrip_across_byte_boundaries) {
    std::mt19937 rng(1234);
    std::vector<std::pair<u32, u32>> written;
    u8 buffer[4096];
    BitWriter writer(buffer, sizeof(buffer));

    for (int i = 0; i < 500; ++i) {
        u32 bits = 1 + (rng() % 32);
        u32 value = (bits >= 32) ? rng() : (rng() & ((1u << bits) - 1u));
        written.push_back({value, bits});
        writer.write_bits(value, bits);
    }
    CHECK(!writer.overflowed());

    BitReader reader(buffer, writer.bytes_written());
    for (const auto& [value, bits] : written) {
        CHECK_EQ(reader.read_bits(bits), value);
    }
    CHECK(!reader.overflowed());
}

TEST_CASE(ranged_values_use_minimum_bits) {
    CHECK_EQ(BitWriter::bits_required(0, 1), 1u);
    CHECK_EQ(BitWriter::bits_required(0, 255), 8u);
    CHECK_EQ(BitWriter::bits_required(0, 256), 9u);
    CHECK_EQ(BitWriter::bits_required(-100, 100), 8u);

    u8 buffer[256];
    BitWriter writer(buffer, sizeof(buffer));
    for (i32 v = -100; v <= 100; ++v) writer.write_ranged(v, -100, 100);
    CHECK(!writer.overflowed());
    CHECK_EQ(writer.bits_written(), 201u * 8u);

    BitReader reader(buffer, writer.bytes_written());
    for (i32 v = -100; v <= 100; ++v) CHECK_EQ(reader.read_ranged(-100, 100), v);
}

TEST_CASE(varint_roundtrip_and_compactness) {
    u8 buffer[256];
    BitWriter writer(buffer, sizeof(buffer));
    const u32 values[] = {0, 1, 127, 128, 300, 16383, 16384, 1000000, 0xffffffff};
    for (u32 v : values) writer.write_varint(v);

    BitReader reader(buffer, writer.bytes_written());
    for (u32 v : values) CHECK_EQ(reader.read_varint(), v);

    BitWriter small(buffer, sizeof(buffer));
    small.write_varint(127);
    CHECK_EQ(small.bits_written(), 8u);
}

TEST_CASE(reading_the_length_mid_stream_does_not_disturb_the_bits) {
    u8 buffer[64];
    BitWriter writer(buffer, sizeof(buffer));
    writer.write_bits(0x2b, 6);
    CHECK_EQ(writer.bytes_written(), 1u);
    CHECK_EQ(writer.bytes_written(), 1u);
    writer.write_bits(0x1d3, 9);
    writer.write_bool(true);
    CHECK_EQ(writer.bytes_written(), 2u);

    BitReader reader(buffer, writer.bytes_written());
    CHECK_EQ(reader.read_bits(6), 0x2bu);
    CHECK_EQ(reader.read_bits(9), 0x1d3u);
    CHECK_EQ(reader.read_bool(), true);
}

TEST_CASE(writer_refuses_to_exceed_capacity) {
    u8 buffer[4];
    BitWriter writer(buffer, sizeof(buffer));
    writer.write_u32(0xdeadbeef);
    CHECK(!writer.overflowed());
    writer.write_bool(true);
    CHECK(writer.overflowed());
    CHECK_EQ(writer.bytes_written(), 4u);
}

TEST_CASE(reader_survives_truncated_packet) {
    u8 buffer[8] = {0};
    BitReader reader(buffer, 2);
    reader.read_u16();
    CHECK(!reader.overflowed());
    reader.read_u32();
    CHECK(reader.overflowed());

    BitReader string_reader(buffer, 2);
    std::string s = string_reader.read_string();
    CHECK(s.empty());
}

TEST_CASE(quantizer_snap_is_idempotent_and_within_tolerance) {
    Quantizer q{-1024.f, 1024.f, 16};
    f32 step = (q.max - q.min) / static_cast<f32>((1u << q.bits) - 1u);

    std::mt19937 rng(99);
    std::uniform_real_distribution<f32> dist(q.min, q.max);
    for (int i = 0; i < 2000; ++i) {
        f32 value = dist(rng);
        f32 snapped = q.snap(value);
        CHECK(std::fabs(snapped - value) <= step);
        CHECK_EQ(q.snap(snapped), snapped);
        CHECK_EQ(q.encode(snapped), q.encode(value));
    }
}

TEST_CASE(quantizer_clamps_out_of_range_inputs) {
    Quantizer q{0.f, 100.f, 10};
    CHECK_NEAR(q.snap(-500.f), 0.f, 0.001);
    CHECK_NEAR(q.snap(9999.f), 100.f, 0.001);
    CHECK_EQ(q.encode(100.f), (1u << 10) - 1u);
    CHECK_EQ(q.encode(0.f), 0u);
}

TEST_CASE(string_and_bytes_roundtrip) {
    u8 buffer[512];
    BitWriter writer(buffer, sizeof(buffer));
    writer.write_bool(true);
    writer.write_string("shard-a:7401");
    const u8 token[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    writer.write_bytes(token, sizeof(token));

    BitReader reader(buffer, writer.bytes_written());
    CHECK(reader.read_bool());
    CHECK(reader.read_string() == "shard-a:7401");
    u8 out[16];
    reader.read_bytes(out, sizeof(out));
    for (int i = 0; i < 16; ++i) CHECK_EQ(out[i], token[i]);
}

TEST_CASE(byte_aligned_payloads_survive_bits_written_around_them) {
    u8 payload[300];
    for (u32 i = 0; i < sizeof(payload); ++i) payload[i] = static_cast<u8>(i * 7 + 3);

    u8 buffer[512];
    BitWriter writer(buffer, sizeof(buffer));
    writer.write_u16(0xbeef);
    writer.write_bytes(payload, sizeof(payload));
    CHECK_EQ(writer.bytes_written(), 302u);
    writer.write_bits(0x5, 3);
    writer.write_u32(0xdeadbeef);
    CHECK_EQ(writer.bytes_written(), 307u);

    BitReader reader(buffer, writer.bytes_written());
    CHECK_EQ(reader.read_u16(), 0xbeefu);
    u8 out[300] = {};
    reader.read_bytes(out, sizeof(out));
    for (u32 i = 0; i < sizeof(payload); ++i) CHECK_EQ(out[i], payload[i]);
    CHECK_EQ(reader.read_bits(3), 0x5u);
    CHECK_EQ(reader.read_u32(), 0xdeadbeefu);
    CHECK(!reader.overflowed());
}

TEST_CASE(a_payload_past_the_end_overflows_rather_than_writing_out_of_bounds) {
    u8 payload[64] = {};
    u8 buffer[32];
    BitWriter writer(buffer, sizeof(buffer));
    writer.write_u16(1);
    writer.write_bytes(payload, sizeof(payload));
    CHECK(writer.overflowed());
    CHECK_EQ(writer.bytes_written(), 2u);

    BitReader reader(buffer, sizeof(buffer));
    u8 out[64];
    reader.read_bytes(out, sizeof(out));
    CHECK(reader.overflowed());
}

TEST_MAIN()
