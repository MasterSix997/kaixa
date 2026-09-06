#pragma once

#include <kaixa/config/build_configuration.hpp>
#include <kaixa/config/value.hpp>
#include <kaixa/model/automation.hpp>
#include <kaixa/model/file_set.hpp>
#include <kaixa/model/version.hpp>
#include <kaixa/test/adapter.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace kaixa {
    enum class DependencyVisibility {
        private_dependency,
        public_dependency
    };

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

    class CandidateSelection {
    public:
        struct Automatic {};
        struct Provider {
            std::string name;
        };
        struct Path {
            std::filesystem::path value;
        };
        struct Source {
            SourceLocator value;
        };

        CandidateSelection() = default;

        [[nodiscard]] static CandidateSelection from_provider(std::string name) { return CandidateSelection(Provider{std::move(name)}); }

        [[nodiscard]] static CandidateSelection from_path(std::filesystem::path path) { return CandidateSelection(Path{std::move(path)}); }

        [[nodiscard]] static CandidateSelection from_source(SourceLocator source) { return CandidateSelection(Source{std::move(source)}); }

        [[nodiscard]] bool automatic() const noexcept { return std::holds_alternative<Automatic>(m_value); }

        [[nodiscard]] const std::string* provider() const noexcept {
            const Provider* selected = std::get_if<Provider>(&m_value);
            return selected ? &selected->name : nullptr;
        }

        [[nodiscard]] const std::filesystem::path* path() const noexcept {
            const Path* selected = std::get_if<Path>(&m_value);
            return selected ? &selected->value : nullptr;
        }

        [[nodiscard]] const SourceLocator* source() const noexcept {
            const Source* selected = std::get_if<Source>(&m_value);
            return selected ? &selected->value : nullptr;
        }

    private:
        explicit CandidateSelection(Provider selected)
            : m_value(std::move(selected)) {}

        explicit CandidateSelection(Path selected)
            : m_value(std::move(selected)) {}

        explicit CandidateSelection(Source selected)
            : m_value(std::move(selected)) {}

        std::variant<Automatic, Provider, Path, Source> m_value;
    };

    struct DependencyBinding {
        PackageRequest request;
        CandidateSelection selection;
        std::optional<std::string> alias;
        DependencyVisibility visibility = DependencyVisibility::private_dependency;
        SourceLocation location;

        DependencyBinding() = default;
        DependencyBinding(std::string dependency_name, std::filesystem::path dependency_path, SourceLocation source_location = {})
            : request{std::move(dependency_name), std::nullopt, {}, false}
            , selection(CandidateSelection::from_path(std::move(dependency_path)))
            , alias(std::nullopt)
            , visibility(DependencyVisibility::private_dependency)
            , location(std::move(source_location)) {}

        [[nodiscard]] std::string_view local_name() const noexcept {
            return alias ? std::string_view(*alias) : std::string_view(request.package);
        }
    };

    struct FeatureDefinition {
        std::string name;
        std::vector<std::string> features;
        std::vector<std::string> dependencies;
        std::map<std::string, std::vector<std::string>> dependency_features;
        std::vector<std::string> members;
        bool legacy = false;
        SourceLocation location;
    };

    enum class ProductDeclarationKind {
        library,
        executable
    };

    struct ProductDeclaration {
        ProductDeclarationKind kind = ProductDeclarationKind::library;
        Value options;
        SourceLocation location;
    };

    struct PackageSet {
        std::string name;
        std::vector<std::string> members;
        std::vector<std::string> exclude;
        std::vector<std::string> defaults;
        std::optional<Value> policy;
        std::optional<std::filesystem::path> provider_config;
        SourceLocation location;
        SourceLocation defaults_location;
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

    struct TargetMatrixAxis {
        std::string name;
        std::vector<Value> values;
        SourceLocation location;
    };

    struct TargetMatrix {
        std::vector<TargetMatrixAxis> axes;
        SourceLocation location;
    };

    struct PackageTarget {
        PackageTargetKind kind = PackageTargetKind::test;
        bool each_source = false;
        bool partial = false;
        std::optional<std::string> name;
        bool name_template = false;
        std::optional<std::string> display_name;
        std::optional<std::string> description;
        std::optional<std::string> category;
        std::vector<std::string> required_features;
        std::map<std::string, std::vector<std::string>> required_dependency_features;
        std::vector<DependencyBinding> dependencies;
        FileSet sources;
        std::vector<std::string> arguments;
        bool discover = false;
        bool hidden = false;
        bool install = false;
        std::optional<std::string> framework;
        std::optional<TestAdapterInfo> adapter;
        std::optional<Value> policy;
        std::optional<Value> resolver_options;
        std::optional<TargetMatrix> matrix;
        std::vector<TaskDeclaration> commands;
        std::filesystem::path source;
        SourceLocation location;
    };

    struct Manifest {
        std::string name;
        std::optional<Version> version;
        std::string resolver;
        std::vector<DependencyBinding> dependencies;
        std::vector<FeatureDefinition> features;
        std::vector<std::string> default_features;
        std::vector<ProductDeclaration> products;
        std::vector<PackageTargetReference> target_references;
        std::vector<PackageTarget> targets;
        ConfigurationSet configurations;
        std::optional<Value> resolver_options;
        std::vector<TaskDeclaration> commands;
        std::vector<WorkflowDeclaration> workflows;
        std::filesystem::path source;
        SourceLocation location;

        Manifest() = default;
        Manifest(std::string package_name, std::string resolver_name)
            : name(std::move(package_name))
            , resolver(std::move(resolver_name)) {}
    };

    struct ManifestDocument {
        std::optional<Manifest> package;
        std::optional<PackageSet> package_set;
        std::vector<Manifest> inline_members;
        ConfigurationSet configurations;
        std::vector<ProviderDefinition> providers;
        std::map<std::string, std::string> routing;
        std::vector<std::filesystem::path> imports;
        std::filesystem::path source;
    };

    struct ManifestTreeSummary {
        std::size_t documents = 0;
        std::size_t packages = 0;
        std::size_t package_sets = 0;
        std::size_t target_documents = 0;
    };

    struct TargetManifestDocument {
        std::filesystem::path source;
        std::vector<PackageTarget> targets;
    };

    using KaixaDocument = std::variant<ManifestDocument, TargetManifestDocument>;

    struct ManifestTree {
        ManifestTreeSummary summary;
        std::vector<KaixaDocument> documents;
    };

    [[nodiscard]] bool is_valid_identifier(std::string_view name) noexcept;
    [[nodiscard]] bool is_valid_package_name(std::string_view name) noexcept;
    [[nodiscard]] bool is_valid_target_name(std::string_view name) noexcept;
    [[nodiscard]] std::string_view target_manifest_filename(PackageTargetKind kind) noexcept;
    [[nodiscard]] Result<ManifestDocument> parse_manifest_document(const Value& document);
    [[nodiscard]] Result<AutomationDocument> read_automation_document(TableReader& root, bool allow_package_scope = false);
    [[nodiscard]] Result<ManifestDocument> parse_manifest_document_file(const std::filesystem::path& path);
    [[nodiscard]] Result<KaixaDocument> parse_kaixa_document_file(const std::filesystem::path& path);
    [[nodiscard]] Result<ManifestDocument> parse_manifest_document_string(std::string_view text, std::string_view source_name);
    [[nodiscard]] Result<Manifest> parse_manifest(const Value& document);
    [[nodiscard]] Result<Manifest> parse_manifest_file(const std::filesystem::path& path);
    [[nodiscard]] Result<Manifest> parse_manifest_string(std::string_view text, std::string_view source_name);
    [[nodiscard]] Result<std::vector<PackageTarget>> parse_package_targets(
        const Value& document,
        const std::filesystem::path& path,
        PackageTargetKind kind,
        std::string_view resolver
    );
    [[nodiscard]] Result<std::vector<PackageTarget>> parse_package_targets_file(
        const std::filesystem::path& path,
        PackageTargetKind kind,
        std::string_view resolver
    );
    [[nodiscard]] Result<ManifestTree> load_manifest_tree(const std::filesystem::path& root);
    [[nodiscard]] Result<ManifestTreeSummary> validate_manifest_tree(const std::filesystem::path& root);
    [[nodiscard]] Result<std::string> format_manifest(const Manifest& manifest);
    [[nodiscard]] Result<void> write_manifest_file(const std::filesystem::path& path, const Manifest& manifest);
}
