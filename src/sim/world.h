#pragma once
#include <deque>
#include <unordered_map>
#include <vector>

#include "core/types.h"
#include "sim/movement.h"
#include "spatial/grid.h"

namespace morton {

enum class EntityKind : u8 {
    kPlayer = 0,
    kDrifter = 1,
};

/// Structure-of-arrays entity storage. Hot tick loops walk position and velocity
/// as contiguous arrays; the cold per-entity bookkeeping lives in parallel arrays
/// that the integrator never touches.
struct EntityStore {
    std::vector<EntityId> id;
    std::vector<Vec2> position;
    std::vector<Vec2> velocity;
    std::vector<u8> kind;
    std::vector<ClientId> owner;
    std::vector<u32> last_input_sequence;
    std::vector<u32> ai_seed;
    std::unordered_map<EntityId, u32> index_of;

    u32 size() const { return static_cast<u32>(id.size()); }
    bool empty() const { return id.empty(); }

    u32 add(EntityId entity, EntityKind entity_kind, const Vec2& start, ClientId client);

    /// Swap-and-pop removal. Returns false if the entity was already gone.
    bool remove(EntityId entity);

    i64 find(EntityId entity) const {
        auto it = index_of.find(entity);
        return it == index_of.end() ? -1 : static_cast<i64>(it->second);
    }

    void clear();
};

struct WorldStats {
    u32 entity_count = 0;
    u32 player_count = 0;
    u32 inputs_applied = 0;
    u32 inputs_starved = 0;
    u32 collision_pairs = 0;
    u32 peak_cell_occupancy = 0;
    f64 integrate_ms = 0.0;
    f64 grid_ms = 0.0;
    f64 collision_ms = 0.0;
};

/// Per-player command buffer and the reconciliation bookkeeping that goes with it.
struct PlayerSlot {
    EntityId entity = kInvalidEntity;
    ClientId client = 0;
    std::deque<MoveInput> pending;
    MoveInput last_applied;
    u32 last_applied_sequence = 0;
    u32 highest_received_sequence = 0;
    u32 dropped_stale_inputs = 0;
    bool has_applied = false;
};

/// The authoritative simulation. Deliberately knows nothing about sockets: it is
/// driven purely by queued inputs and stepped one fixed tick at a time, which is
/// what makes it testable and replayable.
class World {
public:
    void configure(const WorldParams& params, u32 cells_per_axis);

    EntityId spawn_player(ClientId client, const Vec2& position);
    EntityId spawn_drifter(const Vec2& position);
    void despawn(EntityId entity);

    /// Rejects out-of-order and duplicate commands, then queues the input.
    void queue_input(ClientId client, const MoveInput& input);

    /// Advances exactly one tick.
    void step();

    Tick tick() const { return tick_; }
    void set_tick(Tick tick) { tick_ = tick; }

    const WorldParams& params() const { return params_; }
    const EntityStore& entities() const { return entities_; }
    EntityStore& entities() { return entities_; }
    const SpatialGrid& grid() const { return grid_; }
    const WorldStats& stats() const { return stats_; }

    PlayerSlot* player_of(ClientId client);
    const PlayerSlot* player_of(ClientId client) const;
    u32 player_count() const { return static_cast<u32>(players_.size()); }

    /// Directly installs entity state, used when a player migrates in from
    /// another shard and must resume at the exact position it left with.
    void adopt_entity(EntityId entity, ClientId client, const Vec2& position,
                      const Vec2& velocity, u32 last_input_sequence);

    template <typename Fn>
    void for_each_player(Fn&& fn) {
        for (auto& [client, slot] : players_) fn(slot);
    }

private:
    void apply_inputs();
    void integrate();
    void drive_drifters();
    void resolve_collisions();

    WorldParams params_;
    EntityStore entities_;
    SpatialGrid grid_;
    std::unordered_map<ClientId, PlayerSlot> players_;
    WorldStats stats_;
    Tick tick_ = 0;
    EntityId next_entity_ = 1;
};

/// Deterministic 32-bit hash used for drifter steering, so the same shard state
/// produces the same world evolution on any machine.
inline u32 hash_u32(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

}  // namespace morton
