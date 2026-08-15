#pragma once
#include <string>

#include "cluster/presence.h"
#include "core/types.h"

namespace morton {

constexpr u32 kMigrationVersion = 1;
constexpr u32 kMigrationTicketBytes = 512;

/// Everything the destination shard needs to resume a player mid-stride.
///
/// The input sequence travels with the state: without it the destination would
/// re-apply commands the source already simulated, and the client's replay
/// would disagree with the server on the very first tick after the handoff.
struct MigrationTicket {
    u64 token = 0;
    std::string player_id;
    std::string from_shard;
    std::string to_shard;
    u32 region = 0;
    Tick tick = 0;
    Vec2 position;
    Vec2 velocity;
    u32 last_input_sequence = 0;
    u64 issued_ms = 0;
};

/// Serializes a ticket, appending a checksum over the body.
std::string encode_migration_ticket(const MigrationTicket& ticket);

/// Rejects truncated, corrupt and wrong-version blobs rather than trusting them.
bool decode_migration_ticket(const std::string& blob, MigrationTicket* out);

/// Redis-backed handoff tickets.
///
/// Redemption is an atomic get-and-delete, so a replayed or duplicated redirect
/// cannot spawn the same player onto two shards: the second redeem finds
/// nothing. Tickets carry a short TTL so a client that never arrives leaves no
/// permanent state behind.
class MigrationStore {
public:
    explicit MigrationStore(ClusterRegistry* registry) : registry_(registry) {}

    /// Publishes the ticket and moves presence to the receiving shard in a
    /// single round trip, discarding the ticket if the transfer loses its race.
    /// The tick loop pays this latency inline, so the two commands must not
    /// cost two round trips.
    bool publish_and_transfer(const MigrationTicket& ticket, u32 ttl_ms, u32 presence_ttl_ms);

    bool redeem(u64 token, MigrationTicket* out);
    bool discard(u64 token);

    u64 issued() const { return issued_; }
    u64 redeemed() const { return redeemed_; }
    u64 rejected() const { return rejected_; }

private:
    std::string key(u64 token) const;

    ClusterRegistry* registry_ = nullptr;
    u64 issued_ = 0;
    u64 redeemed_ = 0;
    u64 rejected_ = 0;
};

}  // namespace morton
