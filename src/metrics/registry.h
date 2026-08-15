#pragma once
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "core/types.h"
#include "metrics/histogram.h"

namespace morton {

struct Counter {
    std::atomic<u64> value{0};
    std::string help;

    void add(u64 amount = 1) { value.fetch_add(amount, std::memory_order_relaxed); }
    u64 get() const { return value.load(std::memory_order_relaxed); }
};

struct Gauge {
    std::atomic<i64> raw{0};
    std::string help;

    void set(f64 value) {
        raw.store(static_cast<i64>(value * 1000.0), std::memory_order_relaxed);
    }
    f64 get() const { return static_cast<f64>(raw.load(std::memory_order_relaxed)) / 1000.0; }
};

/// Process-wide metric registry with Prometheus text exposition.
///
/// Handles are looked up once and cached by callers, so the hot path never
/// touches the registry lock; only registration and scraping do.
class MetricsRegistry {
public:
    static MetricsRegistry& instance();

    Counter* counter(const std::string& name, const std::string& help = "");
    Gauge* gauge(const std::string& name, const std::string& help = "");
    Histogram* histogram(const std::string& name, const std::string& help = "");

    /// Static labels attached to every exported series, e.g. shard id.
    void set_label(const std::string& key, const std::string& value);

    std::string expose() const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::unique_ptr<Counter>> counters_;
    std::map<std::string, std::unique_ptr<Gauge>> gauges_;
    std::map<std::string, std::unique_ptr<Histogram>> histograms_;
    std::map<std::string, std::string> help_;
    std::map<std::string, std::string> labels_;
};

}  // namespace morton
