#pragma once

#include <filesystem>
#include <string>

namespace kaixa {
    struct BuildEnvironment {
        std::filesystem::path workspace;
        std::filesystem::path state_root;
        std::string profile = "debug";
    };
}
