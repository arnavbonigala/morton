#pragma once
#include <atomic>
#include <cmath>
#include <vector>

#include "core/types.h"

namespace morton {

/// Latency histogram with geometric buckets.
///
/// Recording is a log, a cast and a relaxed atomic increment, so the simulation
/// thread can measure every tick without the measurement showing up in the
/// measurement. Quantiles are interpolated within the containing bucket, which
/// bounds the error at the bucket growth factor rather than at the bucket width.
class Histogram {
public:
    static constexpr u32 kBucketCount = 160;
    static constexpr f64 kGrowthFactor = 1.15;
    static constexpr f64 kFirstBound = 0.001;

    Histogram() : buckets_(kBucketCount) { reset(); }

    Histogram(const Histogram& other) : buckets_(kBucketCount) {
        for (u32 i = 0; i < kBucketCount; ++i) {
            buckets_[i].store(other.buckets_[i].load(std::memory_order_relaxed),
                              std::memory_order_relaxed);
        }
        count_.store(other.count_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        sum_.store(other.sum_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        max_.store(other.max_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        min_.store(other.min_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }

    void record(f64 value) {
        if (value < 0.0) value = 0.0;
        u32 index = bucket_of(value);
        buckets_[index].fetch_add(1, std::memory_order_relaxed);
        count_.fetch_add(1, std::memory_order_relaxed);

        u64 scaled = static_cast<u64>(value * 1000.0);
        sum_.fetch_add(scaled, std::memory_order_relaxed);

        u64 current_max = max_.load(std::memory_order_relaxed);
        while (scaled > current_max &&
               !max_.compare_exchange_weak(current_max, scaled, std::memory_order_relaxed)) {
        }
        u64 current_min = min_.load(std::memory_order_relaxed);
        while (scaled < current_min &&
               !min_.compare_exchange_weak(current_min, scaled, std::memory_order_relaxed)) {
        }
    }

    void reset() {
        for (auto& bucket : buckets_) bucket.store(0, std::memory_order_relaxed);
        count_.store(0, std::memory_order_relaxed);
        sum_.store(0, std::memory_order_relaxed);
        max_.store(0, std::memory_order_relaxed);
        min_.store(~0ull, std::memory_order_relaxed);
    }

    u64 count() const { return count_.load(std::memory_order_relaxed); }

    f64 sum() const {
        return static_cast<f64>(sum_.load(std::memory_order_relaxed)) / 1000.0;
    }

    f64 mean() const {
        u64 total = count();
        return total == 0 ? 0.0 : sum() / static_cast<f64>(total);
    }

    f64 max() const {
        return static_cast<f64>(max_.load(std::memory_order_relaxed)) / 1000.0;
    }

    f64 min() const {
        u64 value = min_.load(std::memory_order_relaxed);
        return value == ~0ull ? 0.0 : static_cast<f64>(value) / 1000.0;
    }

    f64 quantile(f64 q) const;

    f64 p50() const { return quantile(0.50); }
    f64 p95() const { return quantile(0.95); }
    f64 p99() const { return quantile(0.99); }
    f64 p999() const { return quantile(0.999); }

    static f64 bucket_upper_bound(u32 index) {
        return kFirstBound * std::pow(kGrowthFactor, static_cast<f64>(index));
    }

    u64 bucket_count_at(u32 index) const {
        return buckets_[index].load(std::memory_order_relaxed);
    }

private:
    static u32 bucket_of(f64 value) {
        if (value <= kFirstBound) return 0;
        f64 index = std::log(value / kFirstBound) / std::log(kGrowthFactor);
        i32 clamped = static_cast<i32>(index) + 1;
        if (clamped < 0) return 0;
        if (clamped >= static_cast<i32>(kBucketCount)) return kBucketCount - 1;
        return static_cast<u32>(clamped);
    }

    std::vector<std::atomic<u64>> buckets_;
    std::atomic<u64> count_{0};
    std::atomic<u64> sum_{0};
    std::atomic<u64> max_{0};
    std::atomic<u64> min_{~0ull};
};

}  // namespace morton
