#include "metrics/registry.h"

#include <cstdio>
#include <sstream>

namespace morton {

f64 Histogram::quantile(f64 q) const {
    u64 total = count();
    if (total == 0) return 0.0;

    f64 target = q * static_cast<f64>(total);
    u64 cumulative = 0;

    for (u32 i = 0; i < kBucketCount; ++i) {
        u64 bucket = buckets_[i].load(std::memory_order_relaxed);
        if (bucket == 0) continue;

        if (static_cast<f64>(cumulative + bucket) >= target) {
            f64 lower = i == 0 ? 0.0 : bucket_upper_bound(i - 1);
            f64 upper = bucket_upper_bound(i);
            f64 within = (target - static_cast<f64>(cumulative)) / static_cast<f64>(bucket);
            return lower + (upper - lower) * within;
        }
        cumulative += bucket;
    }
    return bucket_upper_bound(kBucketCount - 1);
}

MetricsRegistry& MetricsRegistry::instance() {
    static MetricsRegistry registry;
    return registry;
}

Counter* MetricsRegistry::counter(const std::string& name, const std::string& help) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = counters_.find(name);
    if (it == counters_.end()) {
        it = counters_.emplace(name, std::make_unique<Counter>()).first;
    }
    if (!help.empty()) help_[name] = help;
    return it->second.get();
}

Gauge* MetricsRegistry::gauge(const std::string& name, const std::string& help) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = gauges_.find(name);
    if (it == gauges_.end()) {
        it = gauges_.emplace(name, std::make_unique<Gauge>()).first;
    }
    if (!help.empty()) help_[name] = help;
    return it->second.get();
}

Histogram* MetricsRegistry::histogram(const std::string& name, const std::string& help) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = histograms_.find(name);
    if (it == histograms_.end()) {
        it = histograms_.emplace(name, std::make_unique<Histogram>()).first;
    }
    if (!help.empty()) help_[name] = help;
    return it->second.get();
}

void MetricsRegistry::set_label(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> guard(mutex_);
    labels_[key] = value;
}

std::string MetricsRegistry::expose() const {
    std::lock_guard<std::mutex> guard(mutex_);

    std::string label_text;
    if (!labels_.empty()) {
        label_text = "{";
        bool first = true;
        for (const auto& [key, value] : labels_) {
            if (!first) label_text += ",";
            label_text += key + "=\"" + value + "\"";
            first = false;
        }
        label_text += "}";
    }

    auto labels_with = [&](const std::string& extra) {
        std::string out = "{";
        bool first = true;
        for (const auto& [key, value] : labels_) {
            if (!first) out += ",";
            out += key + "=\"" + value + "\"";
            first = false;
        }
        if (!extra.empty()) {
            if (!first) out += ",";
            out += extra;
        }
        out += "}";
        return out;
    };

    std::ostringstream out;
    out.setf(std::ios::fixed);

    auto emit_help = [&](const std::string& name, const char* type) {
        auto it = help_.find(name);
        if (it != help_.end()) out << "# HELP " << name << " " << it->second << "\n";
        out << "# TYPE " << name << " " << type << "\n";
    };

    for (const auto& [name, counter] : counters_) {
        emit_help(name, "counter");
        out << name << label_text << " " << counter->get() << "\n";
    }

    for (const auto& [name, gauge] : gauges_) {
        emit_help(name, "gauge");
        out.precision(4);
        out << name << label_text << " " << gauge->get() << "\n";
    }

    for (const auto& [name, histogram] : histograms_) {
        emit_help(name, "histogram");
        u64 cumulative = 0;
        out.precision(6);
        for (u32 i = 0; i < Histogram::kBucketCount; ++i) {
            u64 bucket = histogram->bucket_count_at(i);
            cumulative += bucket;
            if (bucket == 0) continue;
            char bound[64];
            std::snprintf(bound, sizeof(bound), "le=\"%.6f\"", Histogram::bucket_upper_bound(i));
            out << name << "_bucket" << labels_with(bound) << " " << cumulative << "\n";
        }
        out << name << "_bucket" << labels_with("le=\"+Inf\"") << " " << histogram->count() << "\n";
        out.precision(6);
        out << name << "_sum" << label_text << " " << histogram->sum() << "\n";
        out << name << "_count" << label_text << " " << histogram->count() << "\n";

        // Quantiles are exported directly as well: operators reading a dashboard
        // should not have to reimplement bucket interpolation to see p99.
        out.precision(4);
        out << name << "_quantile" << labels_with("quantile=\"0.5\"") << " " << histogram->p50()
            << "\n";
        out << name << "_quantile" << labels_with("quantile=\"0.95\"") << " " << histogram->p95()
            << "\n";
        out << name << "_quantile" << labels_with("quantile=\"0.99\"") << " " << histogram->p99()
            << "\n";
    }

    return out.str();
}

}  // namespace morton
