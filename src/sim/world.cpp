#include "sim/world.h"

#include "core/log.h"
#include "core/time.h"

namespace morton {
namespace {

/// A client that stops sending gets its last command repeated for this long
/// before the entity is allowed to coast. Repeating forever would let a
/// disconnected player keep sprinting across the map.
constexpr u32 kInputRepeatTicks = 6;

constexpr u32 kMaxQueuedInputs = 32;

/// Drifters pick a new heading on this cadence, derived from the tick so the
/// world evolves identically on every shard replaying the same range.
constexpr u32 kDrifterHeadingTicks = 45;

}  // namespace

u32 EntityStore::add(EntityId entity, EntityKind entity_kind, const Vec2& start, ClientId client) {
    u32 index = size();
    id.push_back(entity);
    position.push_back(start);
    velocity.push_back(Vec2());
    kind.push_back(static_cast<u8>(entity_kind));
    owner.push_back(client);
    last_input_sequence.push_back(0);
    ai_seed.push_back(hash_u32(entity * 2654435761u));
    index_of[entity] = index;
    return index;
}

bool EntityStore::remove(EntityId entity) {
    auto it = index_of.find(entity);
    if (it == index_of.end()) return false;

    u32 index = it->second;
    u32 last = size() - 1;

    if (index != last) {
        id[index] = id[last];
        position[index] = position[last];
        velocity[index] = velocity[last];
        kind[index] = kind[last];
        owner[index] = owner[last];
        last_input_sequence[index] = last_input_sequence[last];
        ai_seed[index] = ai_seed[last];
        index_of[id[index]] = index;
    }

    id.pop_back();
    position.pop_back();
    velocity.pop_back();
    kind.pop_back();
    owner.pop_back();
    last_input_sequence.pop_back();
    ai_seed.pop_back();
    index_of.erase(it);
    return true;
}

void EntityStore::clear() {
    id.clear();
    position.clear();
    velocity.clear();
    kind.clear();
    owner.clear();
    last_input_sequence.clear();
    ai_seed.clear();
    index_of.clear();
}

void World::configure(const WorldParams& params, u32 cells_per_axis) {
    params_ = params;
    grid_.configure(params.size, cells_per_axis);
    MORTON_CHECK(params.player_radius * 2.f <= grid_.cell_size(),
                 "collision diameter must fit inside one grid cell");
}

EntityId World::spawn_player(ClientId client, const Vec2& position) {
    EntityId entity = next_entity_++;
    entities_.add(entity, EntityKind::kPlayer, position, client);

    PlayerSlot slot;
    slot.entity = entity;
    slot.client = client;
    players_[client] = std::move(slot);
    return entity;
}

EntityId World::spawn_drifter(const Vec2& position) {
    EntityId entity = next_entity_++;
    entities_.add(entity, EntityKind::kDrifter, position, 0);
    return entity;
}

void World::despawn(EntityId entity) {
    i64 index = entities_.find(entity);
    if (index < 0) return;
    ClientId client = entities_.owner[static_cast<u32>(index)];
    entities_.remove(entity);
    if (client != 0) players_.erase(client);
}

void World::adopt_entity(EntityId entity, ClientId client, const Vec2& position,
                         const Vec2& velocity, u32 last_input_sequence) {
    i64 existing = entities_.find(entity);
    u32 index;
    if (existing < 0) {
        index = entities_.add(entity, EntityKind::kPlayer, position, client);
        if (entity >= next_entity_) next_entity_ = entity + 1;
    } else {
        index = static_cast<u32>(existing);
    }

    entities_.position[index] = position;
    entities_.velocity[index] = velocity;
    entities_.owner[index] = client;
    entities_.last_input_sequence[index] = last_input_sequence;

    PlayerSlot slot;
    slot.entity = entity;
    slot.client = client;
    slot.last_applied_sequence = last_input_sequence;
    slot.highest_received_sequence = last_input_sequence;
    players_[client] = std::move(slot);
}

void World::queue_input(ClientId client, const MoveInput& input) {
    auto it = players_.find(client);
    if (it == players_.end()) return;

    PlayerSlot& slot = it->second;
    if (input.sequence <= slot.highest_received_sequence && slot.highest_received_sequence != 0) {
        ++slot.dropped_stale_inputs;
        return;
    }

    slot.highest_received_sequence = input.sequence;
    if (slot.pending.size() >= kMaxQueuedInputs) slot.pending.pop_front();

    MoveInput quantized = input;
    quantize_input(&quantized);
    slot.pending.push_back(quantized);
}

void World::apply_inputs() {
    const f32 dt = params_.tick_dt();

    for (auto& [client, slot] : players_) {
        i64 index = entities_.find(slot.entity);
        if (index < 0) continue;

        MoveInput input;
        if (!slot.pending.empty()) {
            input = slot.pending.front();
            slot.pending.pop_front();
            slot.last_applied = input;
            slot.last_applied_sequence = input.sequence;
            slot.has_applied = true;
            ++stats_.inputs_applied;
        } else if (slot.has_applied && tick_ - slot.last_applied.tick <= kInputRepeatTicks) {
            input = slot.last_applied;
            ++stats_.inputs_starved;
        } else {
            input.sequence = slot.last_applied_sequence;
            ++stats_.inputs_starved;
        }

        MoveState state{entities_.position[static_cast<u32>(index)],
                        entities_.velocity[static_cast<u32>(index)]};
        step_movement(&state, input, params_, dt);
        entities_.position[static_cast<u32>(index)] = state.position;
        entities_.velocity[static_cast<u32>(index)] = state.velocity;
        entities_.last_input_sequence[static_cast<u32>(index)] = slot.last_applied_sequence;
    }
}

void World::drive_drifters() {
    const f32 dt = params_.tick_dt();
    const u32 heading_epoch = tick_ / kDrifterHeadingTicks;

    for (u32 i = 0; i < entities_.size(); ++i) {
        if (entities_.kind[i] != static_cast<u8>(EntityKind::kDrifter)) continue;

        u32 noise = hash_u32(entities_.ai_seed[i] ^ hash_u32(heading_epoch));
        f32 angle = static_cast<f32>(noise & 0xffff) / 65535.f * 6.2831853f;

        MoveInput input;
        input.move_x = std::cos(angle);
        input.move_y = std::sin(angle);
        quantize_input(&input);

        MoveState state{entities_.position[i], entities_.velocity[i]};
        step_movement(&state, input, params_, dt);
        entities_.position[i] = state.position;
        entities_.velocity[i] = state.velocity;
    }
}

void World::integrate() {
    apply_inputs();
    drive_drifters();
}

void World::resolve_collisions() {
    const f32 diameter = params_.player_radius * 2.f;
    u32 pairs = 0;

    grid_.for_each_pair(entities_.position.data(), diameter, [&](u32 a, u32 b) {
        Vec2 delta = entities_.position[b] - entities_.position[a];
        f32 distance = delta.length();

        Vec2 axis;
        if (distance < 1e-4f) {
            u32 tie_break = hash_u32(entities_.id[a] ^ (entities_.id[b] << 16));
            f32 angle = static_cast<f32>(tie_break & 0xffff) / 65535.f * 6.2831853f;
            axis = Vec2(std::cos(angle), std::sin(angle));
            distance = 0.f;
        } else {
            axis = delta * (1.f / distance);
        }

        f32 overlap = diameter - distance;
        if (overlap <= 0.f) return;

        Vec2 correction = axis * (overlap * 0.5f);
        entities_.position[a] -= correction;
        entities_.position[b] += correction;
        ++pairs;
    });

    stats_.collision_pairs = pairs;

    const f32 low = params_.player_radius;
    const f32 high = params_.size - params_.player_radius;
    for (u32 i = 0; i < entities_.size(); ++i) {
        entities_.position[i].x = clampf(entities_.position[i].x, low, high);
        entities_.position[i].y = clampf(entities_.position[i].y, low, high);
    }
}

void World::step() {
    stats_.inputs_applied = 0;
    stats_.inputs_starved = 0;

    {
        ScopedTimer timer(&stats_.integrate_ms);
        integrate();
    }
    {
        ScopedTimer timer(&stats_.grid_ms);
        grid_.build(entities_.position.data(), entities_.size());
    }
    {
        ScopedTimer timer(&stats_.collision_ms);
        resolve_collisions();
    }

    stats_.entity_count = entities_.size();
    stats_.player_count = static_cast<u32>(players_.size());
    stats_.peak_cell_occupancy = grid_.peak_cell_occupancy();
    ++tick_;
}

PlayerSlot* World::player_of(ClientId client) {
    auto it = players_.find(client);
    return it == players_.end() ? nullptr : &it->second;
}

const PlayerSlot* World::player_of(ClientId client) const {
    auto it = players_.find(client);
    return it == players_.end() ? nullptr : &it->second;
}

}  // namespace morton
