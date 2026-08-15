#pragma once
#include <vector>

#include "core/log.h"
#include "core/types.h"
#include "spatial/morton.h"

namespace morton {

/// Uniform grid over a square world, bucketed by Morton code.
///
/// Rebuilt every tick with a counting sort rather than per-cell vectors: one
/// pass to count, a prefix sum, then one pass to scatter. That is two linear
/// passes with zero allocation in steady state, and it leaves entity indices
/// laid out in Z-order so neighborhood queries walk memory that is already
/// close together.
class SpatialGrid {
public:
    /// `cells_per_axis` must be a power of two so Morton codes tile the array exactly.
    void configure(f32 world_size, u32 cells_per_axis);

    /// Buckets `count` positions. Indices refer back into the caller's arrays.
    void build(const Vec2* positions, u32 count);

    void clear();

    u16 cell_x_of(f32 x) const {
        i32 cell = static_cast<i32>(x * inv_cell_size_);
        if (cell < 0) cell = 0;
        if (cell >= static_cast<i32>(cells_per_axis_)) cell = static_cast<i32>(cells_per_axis_) - 1;
        return static_cast<u16>(cell);
    }

    u32 cell_of(const Vec2& position) const {
        return morton_encode(cell_x_of(position.x), cell_x_of(position.y));
    }

    /// Invokes fn(index) for every entity in cells overlapping the circle. Callers
    /// must still range-check, since a cell may extend past the radius.
    template <typename Fn>
    void query_radius(const Vec2& center, f32 radius, Fn&& fn) const {
        if (cells_per_axis_ == 0) return;
        u16 min_x = cell_x_of(center.x - radius);
        u16 max_x = cell_x_of(center.x + radius);
        u16 min_y = cell_x_of(center.y - radius);
        u16 max_y = cell_x_of(center.y + radius);

        for (u16 y = min_y; y <= max_y; ++y) {
            u32 row = spread_bits(y) << 1;
            for (u16 x = min_x; x <= max_x; ++x) {
                u32 code = row | spread_bits(x);
                u32 begin = cell_start_[code];
                u32 end = cell_start_[code + 1];
                for (u32 i = begin; i < end; ++i) fn(sorted_[i]);
            }
        }
    }

    /// Invokes fn(index_a, index_b) once per unique pair within `radius`, using the
    /// half-neighborhood trick so each pair is visited exactly once.
    template <typename Fn>
    void for_each_pair(const Vec2* positions, f32 radius, Fn&& fn) const {
        if (cells_per_axis_ == 0) return;
        MORTON_CHECK(radius <= cell_size_,
                     "pair radius exceeds cell size; neighbour walk would miss pairs");
        f32 radius_sq = radius * radius;

        for (u16 y = 0; y < cells_per_axis_; ++y) {
            for (u16 x = 0; x < cells_per_axis_; ++x) {
                u32 code = morton_encode(x, y);
                u32 begin = cell_start_[code];
                u32 end = cell_start_[code + 1];
                if (begin == end) continue;

                for (u32 i = begin; i < end; ++i) {
                    for (u32 j = i + 1; j < end; ++j) {
                        if (distance_sq(positions[sorted_[i]], positions[sorted_[j]]) <= radius_sq) {
                            fn(sorted_[i], sorted_[j]);
                        }
                    }
                }

                const int kOffsets[4][2] = {{1, 0}, {-1, 1}, {0, 1}, {1, 1}};
                for (const auto& offset : kOffsets) {
                    i32 nx = static_cast<i32>(x) + offset[0];
                    i32 ny = static_cast<i32>(y) + offset[1];
                    if (nx < 0 || ny < 0 || nx >= static_cast<i32>(cells_per_axis_) ||
                        ny >= static_cast<i32>(cells_per_axis_)) {
                        continue;
                    }
                    u32 neighbor = morton_encode(static_cast<u16>(nx), static_cast<u16>(ny));
                    u32 nbegin = cell_start_[neighbor];
                    u32 nend = cell_start_[neighbor + 1];
                    for (u32 i = begin; i < end; ++i) {
                        for (u32 j = nbegin; j < nend; ++j) {
                            if (distance_sq(positions[sorted_[i]], positions[sorted_[j]]) <=
                                radius_sq) {
                                fn(sorted_[i], sorted_[j]);
                            }
                        }
                    }
                }
            }
        }
    }

    u32 cells_per_axis() const { return cells_per_axis_; }
    f32 cell_size() const { return cell_size_; }
    f32 world_size() const { return world_size_; }
    u32 cell_count() const { return cells_per_axis_ * cells_per_axis_; }
    u32 entity_count() const { return static_cast<u32>(sorted_.size()); }

    u32 occupancy(u32 morton_code) const {
        return cell_start_[morton_code + 1] - cell_start_[morton_code];
    }

    /// Largest number of entities in any single cell, a direct read on how badly
    /// clustering is degrading query cost.
    u32 peak_cell_occupancy() const;

private:
    f32 world_size_ = 0.f;
    f32 cell_size_ = 0.f;
    f32 inv_cell_size_ = 0.f;
    u32 cells_per_axis_ = 0;

    std::vector<u32> cell_start_;
    std::vector<u32> sorted_;
    std::vector<u32> codes_;
    std::vector<u32> cursor_;
};

}  // namespace morton
