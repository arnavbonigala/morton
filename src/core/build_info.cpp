#include "core/build_info.h"

namespace morton {

const char* BuildInfo::version() { return "0.1.0"; }

const char* BuildInfo::compiled_at() { return __DATE__ " " __TIME__; }

std::string BuildInfo::summary() {
    return std::string("morton ") + version() + " (built " + compiled_at() + ")";
}

}  // namespace morton
