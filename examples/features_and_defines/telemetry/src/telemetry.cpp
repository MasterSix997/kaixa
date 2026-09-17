#include <feature_telemetry/telemetry.hpp>

#include <chrono>
#include <string>

#ifndef FEATURE_TELEMETRY_BUILD
#error "FEATURE_TELEMETRY_BUILD must remain private to the telemetry target"
#endif

namespace feature_telemetry {
    std::string message() {
#ifdef FEATURE_TELEMETRY_TIMESTAMPS
        const auto ticks = std::chrono::system_clock::now().time_since_epoch().count();
        return "enabled with timestamps (tick " + std::to_string(ticks) + ')';
#else
        return "enabled without timestamps";
#endif
    }
}
