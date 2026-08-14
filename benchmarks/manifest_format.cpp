#include <kaixa/kaixa.hpp>

#include <chrono>
#include <iostream>
#include <string>

int main() {
    kaixa::Manifest manifest{"benchmark", "cmake"};

    constexpr int iterations = 10'000;
    const auto started = std::chrono::steady_clock::now();
    std::size_t bytes = 0;
    for (int iteration = 0; iteration < iterations; ++iteration) {
        auto formatted = kaixa::format_manifest(manifest);
        if (!formatted) {
            std::cerr << kaixa::format_diagnostic(formatted.error()) << '\n';
            return 1;
        }

        bytes += formatted->size();
    }

    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(elapsed);
    std::cout << iterations << " manifests formatted in " << microseconds.count()
        << " us (" << bytes << " bytes)\n";
}
