#pragma once
#include <chrono>
#include <thread>

#include "core/types.h"

namespace morton {

/// Monotonic microseconds since process start.
inline u64 now_us() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point origin = clock::now();
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - origin).count());
}

inline f64 now_ms() { return static_cast<f64>(now_us()) / 1000.0; }

/// Wall-clock milliseconds since epoch, for cross-process coordination.
inline u64 wall_ms() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count());
}

inline void sleep_us(u64 us) {
    if (us == 0) return;
    std::this_thread::sleep_for(std::chrono::microseconds(us));
}

/// Sleeps most of the way then spins, so tick boundaries land within ~50us.
inline void precise_sleep_until(u64 target_us) {
    u64 now = now_us();
    if (target_us <= now) return;
    u64 remaining = target_us - now;
    if (remaining > 1500) sleep_us(remaining - 1000);
    while (now_us() < target_us) std::this_thread::yield();
}

class ScopedTimer {
public:
    explicit ScopedTimer(f64* out_ms) : out_ms_(out_ms), start_(now_us()) {}
    ~ScopedTimer() { *out_ms_ = static_cast<f64>(now_us() - start_) / 1000.0; }

private:
    f64* out_ms_;
    u64 start_;
};

}  // namespace morton
