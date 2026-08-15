#pragma once
#include "core/types.h"

namespace morton {

/// Z-order curve encoding. Interleaving the bits of a cell's x and y coordinates
/// produces a single key whose numeric order preserves spatial locality, so cells
/// that are near each other in the world are near each other in memory.
///
/// The shifts below are the standard bit-spreading trick: each step doubles the
/// gaps between bits until every bit of a 16-bit coordinate sits in an even slot.
inline u32 spread_bits(u16 value) {
    u32 x = value;
    x = (x | (x << 8)) & 0x00ff00ffu;
    x = (x | (x << 4)) & 0x0f0f0f0fu;
    x = (x | (x << 2)) & 0x33333333u;
    x = (x | (x << 1)) & 0x55555555u;
    return x;
}

inline u16 compact_bits(u32 value) {
    u32 x = value & 0x55555555u;
    x = (x | (x >> 1)) & 0x33333333u;
    x = (x | (x >> 2)) & 0x0f0f0f0fu;
    x = (x | (x >> 4)) & 0x00ff00ffu;
    x = (x | (x >> 8)) & 0x0000ffffu;
    return static_cast<u16>(x);
}

inline u32 morton_encode(u16 cell_x, u16 cell_y) {
    return spread_bits(cell_x) | (spread_bits(cell_y) << 1);
}

inline void morton_decode(u32 code, u16* cell_x, u16* cell_y) {
    *cell_x = compact_bits(code);
    *cell_y = compact_bits(code >> 1);
}

}  // namespace morton
