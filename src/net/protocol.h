#pragma once
#include "core/types.h"

namespace morton {

/// Filters stray traffic and distinguishes protocol revisions.
constexpr u32 kProtocolId = 0x4d525401;  // "MRT" + version

enum class PacketType : u8 {
    kConnectionRequest = 1,
    kChallenge = 2,
    kChallengeResponse = 3,
    kConnectionAccepted = 4,
    kConnectionDenied = 5,
    kPayload = 6,
    kDisconnect = 7,
};

enum class DenyReason : u8 {
    kServerFull = 1,
    kBadToken = 2,
    kWrongRegion = 3,
    kShardDraining = 4,
};

/// Application channels carried inside a payload packet.
enum class Channel : u8 {
    kInput = 1,
    kSnapshot = 2,
    kEvent = 3,
};

/// Reliable event message kinds, all sent on Channel::kEvent.
enum class EventType : u8 {
    kMigrateRedirect = 1,
    kMigrateComplete = 2,
    kWorldConfig = 3,
    kPlayerJoined = 4,
    kPlayerLeft = 5,
    kShardDraining = 6,
};

constexpr u32 kMaxReliablePerPacket = 8;
constexpr u32 kMaxReliableMessageSize = 512;
constexpr u32 kAckWindowSize = 1024;
constexpr u32 kConnectTokenBytes = 32;

}  // namespace morton
