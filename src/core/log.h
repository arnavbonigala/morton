#pragma once
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>

#include "core/time.h"
#include "core/types.h"

namespace morton {

enum class LogLevel : int { kTrace = 0, kDebug = 1, kInfo = 2, kWarn = 3, kError = 4 };

class Log {
public:
    static Log& instance() {
        static Log log;
        return log;
    }

    void set_level(LogLevel level) { level_ = level; }
    LogLevel level() const { return level_; }
    void set_tag(std::string tag) { tag_ = std::move(tag); }

    template <typename... Args>
    void write(LogLevel level, const char* fmt, Args... args) {
        if (static_cast<int>(level) < static_cast<int>(level_)) return;
        char body[2048];
        std::snprintf(body, sizeof(body), fmt, args...);
        std::lock_guard<std::mutex> guard(mutex_);
        std::fprintf(stderr, "[%8.3f][%s][%s] %s\n", now_ms() / 1000.0, level_name(level),
                     tag_.c_str(), body);
        std::fflush(stderr);
    }

private:
    static const char* level_name(LogLevel level) {
        switch (level) {
            case LogLevel::kTrace: return "trace";
            case LogLevel::kDebug: return "debug";
            case LogLevel::kInfo: return "info ";
            case LogLevel::kWarn: return "warn ";
            case LogLevel::kError: return "error";
        }
        return "?????";
    }

    LogLevel level_ = LogLevel::kInfo;
    std::string tag_ = "morton";
    std::mutex mutex_;
};

inline void set_log_tag(std::string tag) { Log::instance().set_tag(std::move(tag)); }

inline bool set_log_level_by_name(std::string_view name) {
    if (name == "trace") { Log::instance().set_level(LogLevel::kTrace); return true; }
    if (name == "debug") { Log::instance().set_level(LogLevel::kDebug); return true; }
    if (name == "info") { Log::instance().set_level(LogLevel::kInfo); return true; }
    if (name == "warn") { Log::instance().set_level(LogLevel::kWarn); return true; }
    if (name == "error") { Log::instance().set_level(LogLevel::kError); return true; }
    return false;
}

#define MORTON_LOG_TRACE(...) ::morton::Log::instance().write(::morton::LogLevel::kTrace, __VA_ARGS__)
#define MORTON_LOG_DEBUG(...) ::morton::Log::instance().write(::morton::LogLevel::kDebug, __VA_ARGS__)
#define MORTON_LOG_INFO(...) ::morton::Log::instance().write(::morton::LogLevel::kInfo, __VA_ARGS__)
#define MORTON_LOG_WARN(...) ::morton::Log::instance().write(::morton::LogLevel::kWarn, __VA_ARGS__)
#define MORTON_LOG_ERROR(...) ::morton::Log::instance().write(::morton::LogLevel::kError, __VA_ARGS__)

#define MORTON_CHECK(cond, ...)                        \
    do {                                               \
        if (!(cond)) {                                 \
            MORTON_LOG_ERROR("CHECK failed: " #cond);  \
            MORTON_LOG_ERROR(__VA_ARGS__);             \
            std::abort();                              \
        }                                              \
    } while (0)

}  // namespace morton
