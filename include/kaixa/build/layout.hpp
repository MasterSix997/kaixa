#pragma once

#include <filesystem>
#include <string>

namespace kaixa {
    struct BuildEnvironment {
        std::filesystem::path workspace;
        std::filesystem::path output;
        std::string profile = "debug";
    };
}
