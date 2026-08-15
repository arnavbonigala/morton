#include "spatial/grid.h"

#include <algorithm>

#include "core/log.h"

namespace morton {

void SpatialGrid::configure(f32 world_size, u32 cells_per_axis) {
    MORTON_CHECK(cells_per_axis > 0 && (cells_per_axis & (cells_per_axis - 1)) == 0,
                 "cells_per_axis must be a power of two");
    MORTON_CHECK(cells_per_axis <= 1024, "cells_per_axis exceeds Morton code range");

    world_size_ = world_size;
    cells_per_axis_ = cells_per_axis;
    cell_size_ = world_size / static_cast<f32>(cells_per_axis);
    inv_cell_size_ = 1.f / cell_size_;

    cell_start_.assign(cell_count() + 1, 0);
    sorted_.clear();
    codes_.clear();
}

void SpatialGrid::clear() {
    std::fill(cell_start_.begin(), cell_start_.end(), 0);
    sorted_.clear();
    codes_.clear();
    cursor_.clear();
}

void SpatialGrid::build(const Vec2* positions, u32 count) {
    if (cells_per_axis_ == 0) return;

    const u32 cells = cell_count();
    cell_start_.assign(cells + 1, 0);
    codes_.resize(count);
    sorted_.resize(count);

    for (u32 i = 0; i < count; ++i) {
        u32 code = cell_of(positions[i]);
        codes_[i] = code;
        ++cell_start_[code + 1];
    }

    for (u32 c = 0; c < cells; ++c) cell_start_[c + 1] += cell_start_[c];

    cursor_.assign(cell_start_.begin(), cell_start_.end() - 1);
    for (u32 i = 0; i < count; ++i) sorted_[cursor_[codes_[i]]++] = i;
}

u32 SpatialGrid::peak_cell_occupancy() const {
    u32 peak = 0;
    for (u32 c = 0; c + 1 < cell_start_.size(); ++c) {
        u32 occupancy = cell_start_[c + 1] - cell_start_[c];
        if (occupancy > peak) peak = occupancy;
    }
    return peak;
}

}  // namespace morton
