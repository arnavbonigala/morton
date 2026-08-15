#pragma once
#include <cstdint>
#include <cmath>
#include <string>

namespace morton {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using f32 = float;
using f64 = double;

using EntityId = u32;
using ClientId = u32;
using Tick = u32;

constexpr EntityId kInvalidEntity = 0;

struct Vec2 {
    f32 x = 0.f;
    f32 y = 0.f;

    Vec2() = default;
    Vec2(f32 ix, f32 iy) : x(ix), y(iy) {}

    Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(f32 s) const { return {x * s, y * s}; }
    Vec2& operator+=(const Vec2& o) { x += o.x; y += o.y; return *this; }
    Vec2& operator-=(const Vec2& o) { x -= o.x; y -= o.y; return *this; }
    Vec2& operator*=(f32 s) { x *= s; y *= s; return *this; }
    bool operator==(const Vec2& o) const { return x == o.x && y == o.y; }

    f32 length_sq() const { return x * x + y * y; }
    f32 length() const { return std::sqrt(length_sq()); }

    Vec2 normalized() const {
        f32 len = length();
        if (len < 1e-6f) return {0.f, 0.f};
        return {x / len, y / len};
    }

    Vec2 clamped(f32 max_len) const {
        f32 len_sq = length_sq();
        if (len_sq <= max_len * max_len) return *this;
        f32 len = std::sqrt(len_sq);
        return {x / len * max_len, y / len * max_len};
    }
};

inline f32 dot(const Vec2& a, const Vec2& b) { return a.x * b.x + a.y * b.y; }

inline f32 distance_sq(const Vec2& a, const Vec2& b) {
    f32 dx = a.x - b.x;
    f32 dy = a.y - b.y;
    return dx * dx + dy * dy;
}

inline f32 lerp(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

inline Vec2 lerp(const Vec2& a, const Vec2& b, f32 t) {
    return {lerp(a.x, b.x, t), lerp(a.y, b.y, t)};
}

inline f32 clampf(f32 v, f32 lo, f32 hi) { return v < lo ? lo : (v > hi ? hi : v); }

/// Circular-safe comparison for wrapping 16-bit sequence numbers.
inline bool sequence_greater(u16 a, u16 b) {
    return ((a > b) && (a - b <= 32768)) || ((a < b) && (b - a > 32768));
}

inline bool sequence_greater(u32 a, u32 b) {
    return ((a > b) && (a - b <= 2147483648u)) || ((a < b) && (b - a > 2147483648u));
}

}  // namespace morton
