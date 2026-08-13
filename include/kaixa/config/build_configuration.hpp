#pragma once

#include <kaixa/config/table_reader.hpp>
#include <kaixa/config/value.hpp>
#include <kaixa/foundation/diagnostic.hpp>

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kaixa {
    struct ResolverConfigurationDefinition {
        std::string resolver;
        Value settings;
    };

    struct ConfigurationDefinition {
        std::string name;
        std::optional<std::string> profile;
        std::vector<ResolverConfigurationDefinition> resolvers;
        SourceLocation location;
        SourceLocation profile_location;
    };

    struct ConfigurationSet {
        std::vector<std::string> defaults;
        std::vector<ConfigurationDefinition> definitions;
    };

    struct ResolverArgumentOverride {
        std::string resolver;
        std::vector<std::string> arguments;
        std::string scope;
    };

    struct ResolverArgumentGroup {
        std::string scope;
        std::vector<std::string> arguments;
    };

    struct ResolverBuildConfiguration {
        std::string resolver;
        std::optional<Value> settings;
        std::vector<std::string> arguments;
        std::vector<ResolverArgumentGroup> scoped_arguments;
    };

    struct EffectiveBuildConfiguration {
        std::string profile = "debug";
        SourceLocation profile_origin;
        std::vector<std::string> selected;
        std::vector<ResolverBuildConfiguration> resolvers;

        [[nodiscard]] const ResolverBuildConfiguration* find(std::string_view resolver) const noexcept;
    };

    [[nodiscard]] Result<ConfigurationSet> read_configuration_set(TableReader& root);
    [[nodiscard]] Result<ConfigurationSet> parse_configuration_file(const std::filesystem::path& path);
    [[nodiscard]] Result<EffectiveBuildConfiguration> resolve_configurations(
        std::span<const ConfigurationSet> layers,
        std::span<const std::string> requested,
        const std::optional<std::string>& profile_override,
        std::span<const ResolverArgumentOverride> argument_overrides
    );
}
