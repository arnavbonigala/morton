#pragma once
#include <array>
#include <string>
#include <vector>

#include "net/protocol.h"
#include "proto/bitstream.h"
#include "sim/movement.h"

namespace morton {

/// Commands are re-sent for several ticks so a single dropped datagram does not
/// leave a gap the server has to stall on. The server discards duplicates by
/// sequence, so redundancy costs bandwidth and nothing else.
constexpr u32 kInputRedundancy = 6;
constexpr u32 kMaxInputsPerPacket = 16;

struct InputPacket {
    Tick client_tick = 0;
    u32 acked_snapshot_tick = 0;
    std::vector<MoveInput> inputs;
};

inline u32 encode_input_packet(const InputPacket& packet, u8* out, u32 capacity) {
    BitWriter writer(out, capacity);
    writer.write_u8(static_cast<u8>(Channel::kInput));
    writer.write_u32(packet.client_tick);
    writer.write_u32(packet.acked_snapshot_tick);

    u32 count = static_cast<u32>(packet.inputs.size());
    if (count > kMaxInputsPerPacket) count = kMaxInputsPerPacket;
    writer.write_ranged(static_cast<i32>(count), 0, static_cast<i32>(kMaxInputsPerPacket));

    const Quantizer& axis = input_axis_quantizer();
    for (u32 i = 0; i < count; ++i) {
        const MoveInput& input = packet.inputs[i];
        writer.write_u32(input.sequence);
        writer.write_bits(axis.encode(input.move_x), axis.bits);
        writer.write_bits(axis.encode(input.move_y), axis.bits);
        writer.write_bool(input.sprint);
    }

    return writer.overflowed() ? 0 : writer.bytes_written();
}

inline bool decode_input_packet(const u8* data, u32 size, InputPacket* out) {
    BitReader reader(data, size);
    if (reader.read_u8() != static_cast<u8>(Channel::kInput)) return false;

    out->client_tick = reader.read_u32();
    out->acked_snapshot_tick = reader.read_u32();
    i32 count = reader.read_ranged(0, static_cast<i32>(kMaxInputsPerPacket));
    if (reader.overflowed() || count < 0) return false;

    const Quantizer& axis = input_axis_quantizer();
    out->inputs.clear();
    out->inputs.reserve(static_cast<std::size_t>(count));
    for (i32 i = 0; i < count; ++i) {
        MoveInput input;
        input.sequence = reader.read_u32();
        input.move_x = axis.decode(reader.read_bits(axis.bits));
        input.move_y = axis.decode(reader.read_bits(axis.bits));
        input.sprint = reader.read_bool();
        out->inputs.push_back(input);
    }

    return !reader.overflowed();
}

struct WorldConfigEvent {
    EntityId entity = kInvalidEntity;
    Tick tick = 0;
    u32 region = 0;
    f32 world_size = 2048.f;
    u32 tick_rate = 30;
    std::string shard_id;
};

struct MigrateRedirectEvent {
    u64 ticket = 0;
    u32 region = 0;
    std::string endpoint;
    std::string shard_id;
};

inline u32 encode_world_config(const WorldConfigEvent& event, u8* out, u32 capacity) {
    BitWriter writer(out, capacity);
    writer.write_u8(static_cast<u8>(EventType::kWorldConfig));
    writer.write_u32(event.entity);
    writer.write_u32(event.tick);
    writer.write_varint(event.region);
    writer.write_float(event.world_size);
    writer.write_varint(event.tick_rate);
    writer.write_string(event.shard_id);
    return writer.overflowed() ? 0 : writer.bytes_written();
}

inline bool decode_world_config(const u8* data, u32 size, WorldConfigEvent* out) {
    BitReader reader(data, size);
    if (reader.read_u8() != static_cast<u8>(EventType::kWorldConfig)) return false;
    out->entity = reader.read_u32();
    out->tick = reader.read_u32();
    out->region = reader.read_varint();
    out->world_size = reader.read_float();
    out->tick_rate = reader.read_varint();
    out->shard_id = reader.read_string();
    return !reader.overflowed();
}

inline u32 encode_migrate_redirect(const MigrateRedirectEvent& event, u8* out, u32 capacity) {
    BitWriter writer(out, capacity);
    writer.write_u8(static_cast<u8>(EventType::kMigrateRedirect));
    writer.write_u64(event.ticket);
    writer.write_varint(event.region);
    writer.write_string(event.endpoint);
    writer.write_string(event.shard_id);
    return writer.overflowed() ? 0 : writer.bytes_written();
}

inline bool decode_migrate_redirect(const u8* data, u32 size, MigrateRedirectEvent* out) {
    BitReader reader(data, size);
    if (reader.read_u8() != static_cast<u8>(EventType::kMigrateRedirect)) return false;
    out->ticket = reader.read_u64();
    out->region = reader.read_varint();
    out->endpoint = reader.read_string();
    out->shard_id = reader.read_string();
    return !reader.overflowed() && !out->endpoint.empty();
}

/// Credential presented during the handshake.
///
/// A session token proves the matchmaker placed this player here; a ticket token
/// proves another shard handed it over. Both are single-purpose, so a stolen
/// migration ticket cannot be replayed as a login.
struct ConnectCredential {
    enum class Kind : u8 { kSession = 0, kMigration = 1 };

    Kind kind = Kind::kSession;
    u64 token = 0;
    std::string player_id;
};

constexpr u32 kMaxPlayerIdBytes = 20;

inline bool encode_credential(const ConnectCredential& credential,
                              std::array<u8, kConnectTokenBytes>* out) {
    if (credential.player_id.empty() || credential.player_id.size() > kMaxPlayerIdBytes) {
        return false;
    }
    out->fill(0);
    BitWriter writer(out->data(), kConnectTokenBytes);
    writer.write_u8(static_cast<u8>(credential.kind));
    writer.write_u64(credential.token);
    writer.write_string(credential.player_id);
    return !writer.overflowed();
}

inline bool decode_credential(const std::array<u8, kConnectTokenBytes>& token,
                              ConnectCredential* out) {
    BitReader reader(token.data(), kConnectTokenBytes);
    u8 kind = reader.read_u8();
    if (kind > static_cast<u8>(ConnectCredential::Kind::kMigration)) return false;
    out->kind = static_cast<ConnectCredential::Kind>(kind);
    out->token = reader.read_u64();
    out->player_id = reader.read_string();
    if (reader.overflowed() || out->player_id.empty() ||
        out->player_id.size() > kMaxPlayerIdBytes) {
        return false;
    }
    return true;
}

}  // namespace morton
