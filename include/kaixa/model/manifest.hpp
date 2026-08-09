#pragma once

#include <kaixa/config/value.hpp>
#include <kaixa/config/build_configuration.hpp>
#include <kaixa/model/version.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kaixa {
    struct DependencySpec {
        std::string name;
        std::filesystem::path path;
        SourceLocation location;
    };

    struct Manifest {
        std::string name;
        std::optional<Version> version;
        std::string resolver;
        std::vector<DependencySpec> dependencies;
        ConfigurationSet configurations;
        std::optional<Value> resolver_options;
        std::filesystem::path source;
        SourceLocation location;
    };

    [[nodiscard]] bool is_valid_identifier(std::string_view name) noexcept;
    [[nodiscard]] Result<Manifest> parse_manifest(const Value& document);
    [[nodiscard]] Result<Manifest> parse_manifest_file(const std::filesystem::path& path);
    [[nodiscard]] Result<Manifest> parse_manifest_string(
        std::string_view text,
        std::string_view source_name
    );
}
