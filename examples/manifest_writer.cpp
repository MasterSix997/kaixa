#include <kaixa/kaixa.hpp>

#include <iostream>

int main() {
    kaixa::Manifest manifest{"hello", "cmake"};
    manifest.version = kaixa::Version{"0.1.0"};

    auto text = kaixa::format_manifest(manifest);
    if (!text) {
        std::cerr << kaixa::format_diagnostic(text.error()) << '\n';
        return 1;
    }

    std::cout << *text;
    return 0;
}
