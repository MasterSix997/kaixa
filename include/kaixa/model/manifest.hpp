#pragma once

#include <kaixa/config/value.hpp>
#include <kaixa/config/build_configuration.hpp>
#include <kaixa/model/version.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kaixa {
    struct DependencySpec {
        std::string name;
        std::filesystem::path path;
        SourceLocation location;

        DependencySpec() = default;
        DependencySpec(
            std::string dependency_name,
            std::filesystem::path dependency_path,
            SourceLocation source_location = {}
        ) : name(std::move(dependency_name)),
            path(std::move(dependency_path)),
            location(std::move(source_location)) {
        }
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

        Manifest() = default;
        Manifest(std::string package_name, std::string resolver_name)
            : name(std::move(package_name)), resolver(std::move(resolver_name)) {
        }
    };

    [[nodiscard]] bool is_valid_identifier(std::string_view name) noexcept;
    [[nodiscard]] Result<Manifest> parse_manifest(const Value& document);
    [[nodiscard]] Result<Manifest> parse_manifest_file(const std::filesystem::path& path);
    [[nodiscard]] Result<Manifest> parse_manifest_string(std::string_view text, std::string_view source_name);
    [[nodiscard]] Result<std::string> format_manifest(const Manifest& manifest);
    [[nodiscard]] Result<void> write_manifest_file(const std::filesystem::path& path, const Manifest& manifest);
}
