#pragma once

#include <kaixa/config/build_configuration.hpp>

#include <filesystem>
#include <string>
#include <utility>

namespace kaixa {
    struct BuildEnvironment {
        BuildEnvironment(
            std::filesystem::path workspace_value,
            std::filesystem::path state_root_value,
            std::string profile_value = "debug"
        )
            : workspace(std::move(workspace_value)),
              state_root(std::move(state_root_value)) {
            configuration.profile = std::move(profile_value);
        }

        BuildEnvironment(
            std::filesystem::path workspace_value,
            std::filesystem::path state_root_value,
            EffectiveBuildConfiguration configuration_value
        )
            : workspace(std::move(workspace_value)),
              state_root(std::move(state_root_value)),
              configuration(std::move(configuration_value)) {
        }

        std::filesystem::path workspace;
        std::filesystem::path state_root;
        EffectiveBuildConfiguration configuration;
    };
}
