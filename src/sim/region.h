#pragma once
#include <string>
#include <vector>

#include "core/types.h"

namespace morton {

constexpr u32 kInvalidRegion = 0xffffffffu;

/// Uniform grid of authority regions over the world.
///
/// Handoff uses hysteresis rather than the raw region boundary: a player is only
/// handed to the neighbouring shard once it is `margin` units past the border,
/// otherwise anyone walking along a seam would ping-pong between shards every
/// tick and pay a full reconnect for each crossing.
struct RegionMap {
    f32 world_size = 2048.f;
    u32 regions_per_axis = 2;
    f32 margin = 48.f;

    u32 region_count() const { return regions_per_axis * regions_per_axis; }

    f32 region_size() const { return world_size / static_cast<f32>(regions_per_axis); }

    u32 axis_index(f32 value) const {
        f32 size = region_size();
        i32 index = static_cast<i32>(value / size);
        if (index < 0) index = 0;
        if (index >= static_cast<i32>(regions_per_axis)) index = static_cast<i32>(regions_per_axis) - 1;
        return static_cast<u32>(index);
    }

    u32 region_of(f32 x, f32 y) const {
        return axis_index(y) * regions_per_axis + axis_index(x);
    }

    void bounds_of(u32 region, f32* min_x, f32* min_y, f32* max_x, f32* max_y) const {
        f32 size = region_size();
        u32 cx = region % regions_per_axis;
        u32 cy = region / regions_per_axis;
        *min_x = static_cast<f32>(cx) * size;
        *min_y = static_cast<f32>(cy) * size;
        *max_x = *min_x + size;
        *max_y = *min_y + size;
    }

    /// Region a player currently at (x, y) should be handed off to, given the
    /// region that owns it now. Returns kInvalidRegion when it should stay put.
    u32 handoff_target(u32 current_region, f32 x, f32 y) const {
        u32 raw = region_of(x, y);
        if (raw == current_region) return kInvalidRegion;

        f32 min_x, min_y, max_x, max_y;
        bounds_of(current_region, &min_x, &min_y, &max_x, &max_y);

        bool past_x = x < min_x - margin || x > max_x + margin;
        bool past_y = y < min_y - margin || y > max_y + margin;
        return (past_x || past_y) ? raw : kInvalidRegion;
    }

    /// Spawn point at the centre of a region.
    void center_of(u32 region, f32* x, f32* y) const {
        f32 min_x, min_y, max_x, max_y;
        bounds_of(region, &min_x, &min_y, &max_x, &max_y);
        *x = (min_x + max_x) * 0.5f;
        *y = (min_y + max_y) * 0.5f;
    }
};

/// Deterministic region-to-shard assignment over a sorted shard list.
///
/// Every process derives the same mapping from the same registry snapshot, so
/// a shard can compute where to hand a player off without asking anyone. When
/// a shard dies its regions are redistributed by the next snapshot rather than
/// by an election.
inline std::vector<std::string> assign_regions(const std::vector<std::string>& sorted_shard_ids,
                                               u32 region_count) {
    std::vector<std::string> owners(region_count);
    if (sorted_shard_ids.empty()) return owners;
    for (u32 region = 0; region < region_count; ++region) {
        owners[region] = sorted_shard_ids[region % sorted_shard_ids.size()];
    }
    return owners;
}

}  // namespace morton
