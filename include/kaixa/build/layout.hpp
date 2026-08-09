#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace kaixa {
    struct BuildEnvironment {
        std::filesystem::path workspace;
        std::filesystem::path state_root;
        std::string profile = "debug";
        std::vector<std::string> resolver_arguments;
    };
}
