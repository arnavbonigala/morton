#include <algorithm>
#include <random>
#include <set>
#include <vector>

#include "spatial/grid.h"
#include "spatial/morton.h"
#include "tests/check.h"

using namespace morton;

namespace {

std::vector<Vec2> random_positions(u32 count, f32 world_size, u32 seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<f32> dist(0.f, world_size);
    std::vector<Vec2> positions(count);
    for (u32 i = 0; i < count; ++i) positions[i] = Vec2(dist(rng), dist(rng));
    return positions;
}

std::set<u32> brute_force_radius(const std::vector<Vec2>& positions, const Vec2& center,
                                 f32 radius) {
    std::set<u32> result;
    for (u32 i = 0; i < positions.size(); ++i) {
        if (distance_sq(positions[i], center) <= radius * radius) result.insert(i);
    }
    return result;
}

}  // namespace

TEST_CASE(morton_encode_decode_roundtrips) {
    std::mt19937 rng(5);
    for (int i = 0; i < 10000; ++i) {
        u16 x = static_cast<u16>(rng() & 0xffff);
        u16 y = static_cast<u16>(rng() & 0xffff);
        u32 code = morton_encode(x, y);
        u16 out_x, out_y;
        morton_decode(code, &out_x, &out_y);
        CHECK_EQ(out_x, x);
        CHECK_EQ(out_y, y);
    }
}

TEST_CASE(morton_codes_are_distinct_and_interleaved) {
    std::set<u32> seen;
    for (u16 y = 0; y < 64; ++y) {
        for (u16 x = 0; x < 64; ++x) {
            u32 code = morton_encode(x, y);
            CHECK(seen.insert(code).second);
            CHECK(code < 64u * 64u);
        }
    }
    CHECK_EQ(morton_encode(0, 0), 0u);
    CHECK_EQ(morton_encode(1, 0), 1u);
    CHECK_EQ(morton_encode(0, 1), 2u);
    CHECK_EQ(morton_encode(1, 1), 3u);
}

TEST_CASE(morton_order_preserves_locality_better_than_row_major) {
    const u32 kAxis = 32;
    std::mt19937 rng(11);
    u64 morton_jumps = 0;
    u64 row_major_jumps = 0;

    for (int trial = 0; trial < 2000; ++trial) {
        u16 x = static_cast<u16>(rng() % (kAxis - 1));
        u16 y = static_cast<u16>(rng() % (kAxis - 1));
        u32 m_here = morton_encode(x, y);
        u32 m_below = morton_encode(x, y + 1);
        morton_jumps += m_below > m_here ? m_below - m_here : m_here - m_below;

        u32 r_here = y * kAxis + x;
        u32 r_below = (y + 1) * kAxis + x;
        row_major_jumps += r_below - r_here;
    }

    CHECK(morton_jumps < row_major_jumps);
}

TEST_CASE(radius_query_matches_brute_force) {
    const f32 kWorld = 1024.f;
    SpatialGrid grid;
    grid.configure(kWorld, 32);

    std::vector<Vec2> positions = random_positions(4000, kWorld, 17);
    grid.build(positions.data(), static_cast<u32>(positions.size()));
    CHECK_EQ(grid.entity_count(), 4000u);

    std::mt19937 rng(23);
    std::uniform_real_distribution<f32> coord(0.f, kWorld);

    for (int trial = 0; trial < 200; ++trial) {
        Vec2 center(coord(rng), coord(rng));
        f32 radius = 10.f + static_cast<f32>(rng() % 90);

        std::set<u32> from_grid;
        grid.query_radius(center, radius, [&](u32 index) {
            if (distance_sq(positions[index], center) <= radius * radius) from_grid.insert(index);
        });

        CHECK(from_grid == brute_force_radius(positions, center, radius));
    }
}

TEST_CASE(radius_query_visits_far_fewer_entities_than_a_linear_scan) {
    const f32 kWorld = 1024.f;
    SpatialGrid grid;
    grid.configure(kWorld, 32);

    std::vector<Vec2> positions = random_positions(10000, kWorld, 41);
    grid.build(positions.data(), static_cast<u32>(positions.size()));

    u64 visited = 0;
    std::mt19937 rng(43);
    std::uniform_real_distribution<f32> coord(0.f, kWorld);
    const int kTrials = 200;
    for (int trial = 0; trial < kTrials; ++trial) {
        Vec2 center(coord(rng), coord(rng));
        grid.query_radius(center, 40.f, [&](u32) { ++visited; });
    }

    u64 linear_scan = static_cast<u64>(positions.size()) * kTrials;
    CHECK(visited * 20 < linear_scan);
}

TEST_CASE(pair_iteration_matches_brute_force_exactly_once) {
    const f32 kWorld = 512.f;
    const f32 kRadius = 12.f;
    SpatialGrid grid;
    grid.configure(kWorld, 32);
    CHECK(kRadius <= grid.cell_size());

    std::vector<Vec2> positions = random_positions(3000, kWorld, 71);
    grid.build(positions.data(), static_cast<u32>(positions.size()));

    std::set<std::pair<u32, u32>> from_grid;
    u64 emitted = 0;
    grid.for_each_pair(positions.data(), kRadius, [&](u32 a, u32 b) {
        ++emitted;
        from_grid.insert({std::min(a, b), std::max(a, b)});
    });

    std::set<std::pair<u32, u32>> expected;
    for (u32 i = 0; i < positions.size(); ++i) {
        for (u32 j = i + 1; j < positions.size(); ++j) {
            if (distance_sq(positions[i], positions[j]) <= kRadius * kRadius) {
                expected.insert({i, j});
            }
        }
    }

    CHECK(from_grid == expected);
    CHECK_EQ(emitted, static_cast<u64>(from_grid.size()));
    CHECK(!expected.empty());
}

TEST_CASE(entities_outside_world_bounds_are_clamped_into_edge_cells) {
    SpatialGrid grid;
    grid.configure(256.f, 16);

    std::vector<Vec2> positions = {Vec2(-500.f, -500.f), Vec2(9999.f, 9999.f), Vec2(128.f, 128.f)};
    grid.build(positions.data(), 3);

    CHECK_EQ(grid.entity_count(), 3u);
    std::set<u32> found;
    grid.query_radius(Vec2(0.f, 0.f), 20.f, [&](u32 i) { found.insert(i); });
    CHECK(found.count(0) == 1);
}

TEST_CASE(rebuild_reflects_movement_and_does_not_leak_stale_entries) {
    SpatialGrid grid;
    grid.configure(256.f, 16);

    std::vector<Vec2> positions = {Vec2(10.f, 10.f)};
    grid.build(positions.data(), 1);

    u32 count_near_origin = 0;
    grid.query_radius(Vec2(10.f, 10.f), 5.f, [&](u32) { ++count_near_origin; });
    CHECK_EQ(count_near_origin, 1u);

    positions[0] = Vec2(240.f, 240.f);
    grid.build(positions.data(), 1);

    count_near_origin = 0;
    grid.query_radius(Vec2(10.f, 10.f), 5.f, [&](u32) { ++count_near_origin; });
    CHECK_EQ(count_near_origin, 0u);

    u32 count_far = 0;
    grid.query_radius(Vec2(240.f, 240.f), 5.f, [&](u32) { ++count_far; });
    CHECK_EQ(count_far, 1u);
}

TEST_CASE(peak_occupancy_detects_clustering) {
    SpatialGrid grid;
    grid.configure(1024.f, 32);

    std::vector<Vec2> spread = random_positions(3200, 1024.f, 3);
    grid.build(spread.data(), static_cast<u32>(spread.size()));
    u32 spread_peak = grid.peak_cell_occupancy();

    std::vector<Vec2> clustered(3200, Vec2(500.f, 500.f));
    grid.build(clustered.data(), static_cast<u32>(clustered.size()));
    CHECK_EQ(grid.peak_cell_occupancy(), 3200u);
    CHECK(spread_peak < 100u);
}

TEST_MAIN()
