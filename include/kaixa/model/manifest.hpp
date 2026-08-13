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
            std::string name,
            std::filesystem::path path,
            SourceLocation location = {}
        ) : name(std::move(name)), path(std::move(path)), location(std::move(location)) {
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
        Manifest(std::string name, std::string resolver)
            : name(std::move(name)), resolver(std::move(resolver)) {
        }
    };

    [[nodiscard]] bool is_valid_identifier(std::string_view name) noexcept;
    [[nodiscard]] Result<Manifest> parse_manifest(const Value& document);
    [[nodiscard]] Result<Manifest> parse_manifest_file(const std::filesystem::path& path);
    [[nodiscard]] Result<Manifest> parse_manifest_string(std::string_view text, std::string_view source_name);
    [[nodiscard]] Result<std::string> format_manifest(const Manifest& manifest);
    [[nodiscard]] Result<void> write_manifest_file(const std::filesystem::path& path, const Manifest& manifest);
}
