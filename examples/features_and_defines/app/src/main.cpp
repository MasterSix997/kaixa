#include <iostream>

#ifdef FEATURE_DEMO_DIAGNOSTICS
#include <feature_telemetry/telemetry.hpp>
#endif

#ifndef FEATURE_DEMO_API
#error "FEATURE_DEMO_API must be defined by Kaixa"
#endif

#define FEATURE_DEMO_STRINGIFY_INNER(value) #value
#define FEATURE_DEMO_STRINGIFY(value) FEATURE_DEMO_STRINGIFY_INNER(value)

int main() {
    std::cout << "app: " << FEATURE_DEMO_STRINGIFY(FEATURE_DEMO_NAME) << '\n';
    std::cout << "api: " << FEATURE_DEMO_API << '\n';
    std::cout << "assets: " << FEATURE_DEMO_STRINGIFY(FEATURE_DEMO_ASSET_DIR) << '\n';

#ifdef FEATURE_DEMO_FRIENDLY
    std::cout << "friendly-output: enabled\n";
#else
    std::cout << "friendly-output: disabled\n";
#endif

#if FEATURE_DEMO_EXPERIMENTAL
    std::cout << "experimental: enabled\n";
#else
    std::cout << "experimental: disabled\n";
#endif

#ifdef FEATURE_DEMO_DIAGNOSTICS
    std::cout << "diagnostics: " << feature_telemetry::message() << '\n';
#else
    std::cout << "diagnostics: disabled (optional dependency not selected)\n";
#endif
}
