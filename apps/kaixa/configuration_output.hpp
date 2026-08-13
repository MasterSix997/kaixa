#pragma once

#include <kaixa/kaixa.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace kaixa::cli {
    struct ConfigurationSource {
        std::string name;
        ConfigurationSet configurations;
    };

    void print_configuration_list(
        const std::vector<ConfigurationSource>& sources,
        const EffectiveBuildConfiguration& configuration,
        const std::filesystem::path& workspace
    );

    void print_effective_configuration(
        const EffectiveBuildConfiguration& configuration,
        const std::vector<ConfigurationSource>& sources,
        std::string_view root_resolver,
        const std::filesystem::path& workspace
    );
}
