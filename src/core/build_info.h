#pragma once
#include <string>

namespace morton {

/// Version and build stamp, reported by every service on startup and via metrics.
struct BuildInfo {
    static const char* version();
    static const char* compiled_at();
    static std::string summary();
};

}  // namespace morton
