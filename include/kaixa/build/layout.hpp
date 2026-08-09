#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace kaixa {
    struct BuildEnvironment {
        BuildEnvironment(
            std::filesystem::path workspace_value,
            std::filesystem::path state_root_value,
            std::string profile_value = "debug",
            std::vector<std::string> resolver_arguments_value = {}
        )
            : workspace(std::move(workspace_value)),
              state_root(std::move(state_root_value)),
              profile(std::move(profile_value)),
              resolver_arguments(std::move(resolver_arguments_value)) {
        }

        std::filesystem::path workspace;
        std::filesystem::path state_root;
        std::string profile;
        std::vector<std::string> resolver_arguments;
    };
}
