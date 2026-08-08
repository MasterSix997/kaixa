#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace kaixa {
    struct Action {
        std::string description;
        std::vector<std::string> argv;
        std::filesystem::path working_directory;
        std::vector<std::filesystem::path> inputs;
        std::vector<std::filesystem::path> outputs;
    };
}
