#pragma once

#include <kaixa/config/value.hpp>
#include <kaixa/config/build_configuration.hpp>
#include <kaixa/model/file_set.hpp>
#include <kaixa/model/version.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kaixa {
    struct PackageRequest {
        std::string package;
        std::optional<VersionRequirement> version;
        std::vector<std::string> features;
        bool optional = false;
    };

    struct SourceLocator {
        std::string driver;
        Value options;
    };

    struct CandidateSelection {
        std::optional<std::string> provider;
        std::optional<std::filesystem::path> path;
        std::optional<SourceLocator> source;
    };

    struct DependencyBinding {
        PackageRequest request;
        CandidateSelection selection;
        std::optional<std::string> alias;
        SourceLocation location;

        DependencyBinding() = default;
        DependencyBinding(
            std::string dependency_name,
            std::filesystem::path dependency_path,
            SourceLocation source_location = {}
        ) : request{std::move(dependency_name), std::nullopt, {}, false},
            selection{std::nullopt, std::move(dependency_path), std::nullopt},
            alias(std::nullopt),
            location(std::move(source_location)) {
        }

        [[nodiscard]] std::string_view local_name() const noexcept {
            return alias ? std::string_view(*alias) : std::string_view(request.package);
        }
    };

    struct PackageSet {
        std::vector<std::string> members;
        std::vector<std::string> exclude;
        std::vector<std::string> defaults;
        SourceLocation location;
    };

    enum class PackageTargetKind {
        test,
        example,
        benchmark
    };

    struct PackageTargetReference {
        PackageTargetKind kind = PackageTargetKind::test;
        std::filesystem::path path;
        SourceLocation location;
    };

    struct PackageTarget {
        PackageTargetKind kind = PackageTargetKind::test;
        bool each_source = false;
        std::optional<std::string> name;
        std::optional<std::string> display_name;
        std::optional<std::string> description;
        std::optional<std::string> category;
        std::vector<std::string> required_features;
        std::vector<DependencyBinding> dependencies;
        FileSet sources;
        std::vector<std::string> arguments;
        bool discover = false;
        bool hidden = false;
        std::optional<Value> resolver_options;
        std::filesystem::path source;
        SourceLocation location;
    };

    struct Manifest {
        std::string name;
        std::optional<Version> version;
        std::string resolver;
        std::vector<DependencyBinding> dependencies;
        std::vector<PackageTargetReference> target_references;
        std::vector<PackageTarget> targets;
        std::vector<PackageTarget> resolved_targets;
        ConfigurationSet configurations;
        std::optional<Value> resolver_options;
        std::filesystem::path source;
        SourceLocation location;

        Manifest() = default;
        Manifest(std::string package_name, std::string resolver_name)
            : name(std::move(package_name)), resolver(std::move(resolver_name)) {
        }
    };

    struct ManifestDocument {
        std::optional<Manifest> package;
        std::optional<PackageSet> package_set;
        ConfigurationSet configurations;
        std::filesystem::path source;
    };

    [[nodiscard]] bool is_valid_identifier(std::string_view name) noexcept;
    [[nodiscard]] bool is_valid_package_name(std::string_view name) noexcept;
    [[nodiscard]] bool is_valid_target_name(std::string_view name) noexcept;
    [[nodiscard]] Result<ManifestDocument> parse_manifest_document(const Value& document);
    [[nodiscard]] Result<ManifestDocument> parse_manifest_document_file(const std::filesystem::path& path);
    [[nodiscard]] Result<ManifestDocument> parse_manifest_document_string(std::string_view text, std::string_view source_name);
    [[nodiscard]] Result<Manifest> parse_manifest(const Value& document);
    [[nodiscard]] Result<Manifest> parse_manifest_file(const std::filesystem::path& path);
    [[nodiscard]] Result<Manifest> parse_manifest_string(std::string_view text, std::string_view source_name);
    [[nodiscard]] Result<std::vector<PackageTarget>> parse_package_targets_file(const std::filesystem::path& path, PackageTargetKind kind, std::string_view resolver);
    [[nodiscard]] Result<std::string> format_manifest(const Manifest& manifest);
    [[nodiscard]] Result<void> write_manifest_file(const std::filesystem::path& path, const Manifest& manifest);
}
