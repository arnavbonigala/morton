#include "cluster/migration.h"

#include "core/log.h"
#include "core/time.h"
#include "proto/bitstream.h"

namespace morton {
namespace {

u32 checksum(const u8* data, u32 size) {
    u32 hash = 2166136261u;
    for (u32 i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

}  // namespace

std::string encode_migration_ticket(const MigrationTicket& ticket) {
    u8 buffer[kMigrationTicketBytes];
    BitWriter writer(buffer, kMigrationTicketBytes);

    writer.write_varint(kMigrationVersion);
    writer.write_u64(ticket.token);
    writer.write_string(ticket.player_id);
    writer.write_string(ticket.from_shard);
    writer.write_string(ticket.to_shard);
    writer.write_varint(ticket.region);
    writer.write_u32(ticket.tick);
    writer.write_float(ticket.position.x);
    writer.write_float(ticket.position.y);
    writer.write_float(ticket.velocity.x);
    writer.write_float(ticket.velocity.y);
    writer.write_varint(ticket.last_input_sequence);
    writer.write_u64(ticket.issued_ms);
    writer.align();

    if (writer.overflowed()) return "";

    u32 body = writer.bytes_written();
    u32 sum = checksum(buffer, body);
    writer.write_u32(sum);
    if (writer.overflowed()) return "";

    return std::string(reinterpret_cast<const char*>(buffer), writer.bytes_written());
}

bool decode_migration_ticket(const std::string& blob, MigrationTicket* out) {
    if (blob.size() < 5 || blob.size() > kMigrationTicketBytes) return false;

    const u8* data = reinterpret_cast<const u8*>(blob.data());
    u32 body = static_cast<u32>(blob.size()) - 4;

    u32 expected = static_cast<u32>(data[body]) | (static_cast<u32>(data[body + 1]) << 8) |
                   (static_cast<u32>(data[body + 2]) << 16) |
                   (static_cast<u32>(data[body + 3]) << 24);
    if (checksum(data, body) != expected) return false;

    BitReader reader(data, body);
    if (reader.read_varint() != kMigrationVersion) return false;

    MigrationTicket ticket;
    ticket.token = reader.read_u64();
    ticket.player_id = reader.read_string();
    ticket.from_shard = reader.read_string();
    ticket.to_shard = reader.read_string();
    ticket.region = reader.read_varint();
    ticket.tick = reader.read_u32();
    ticket.position.x = reader.read_float();
    ticket.position.y = reader.read_float();
    ticket.velocity.x = reader.read_float();
    ticket.velocity.y = reader.read_float();
    ticket.last_input_sequence = reader.read_varint();
    ticket.issued_ms = reader.read_u64();

    if (reader.overflowed() || ticket.player_id.empty()) return false;

    *out = ticket;
    return true;
}

std::string MigrationStore::key(u64 token) const {
    return registry_->prefix() + ":ticket:" + std::to_string(token);
}

bool MigrationStore::publish(const MigrationTicket& ticket, u32 ttl_ms) {
    MigrationTicket stamped = ticket;
    if (stamped.issued_ms == 0) stamped.issued_ms = wall_ms();

    std::string blob = encode_migration_ticket(stamped);
    if (blob.empty()) {
        MORTON_LOG_ERROR("migration ticket for %s exceeds %u bytes",
                         stamped.player_id.c_str(), kMigrationTicketBytes);
        return false;
    }

    RedisReply reply = registry_->client().command(
        {"SET", key(stamped.token), blob, "PX", std::to_string(ttl_ms), "NX"});
    if (!reply.ok()) return false;

    ++issued_;
    return true;
}

bool MigrationStore::redeem(u64 token, MigrationTicket* out) {
    RedisReply reply = registry_->client().command({"GETDEL", key(token)});
    if (reply.type != RedisType::kString || !decode_migration_ticket(reply.str, out)) {
        ++rejected_;
        return false;
    }
    if (out->token != token) {
        ++rejected_;
        return false;
    }
    ++redeemed_;
    return true;
}

bool MigrationStore::discard(u64 token) {
    RedisReply reply = registry_->client().command({"DEL", key(token)});
    return reply.type == RedisType::kInteger && reply.integer == 1;
}

}  // namespace morton
