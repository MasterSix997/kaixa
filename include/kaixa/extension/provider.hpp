#pragma once

#include <kaixa/config/provider_configuration.hpp>
#include <kaixa/foundation/diagnostic.hpp>
#include <kaixa/model/manifest.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace kaixa {
    struct ProviderInfo {
        std::string name;
        std::string driver;
        bool is_default = false;
    };

    struct PackageCandidate {
        std::string package;
        std::optional<Version> version;
        std::string authority;
        std::optional<SourceLocator> source;
        std::optional<std::string> resolver;
        std::optional<Value> descriptor;
        std::optional<SourceLocator> artifact;
    };

    struct ProviderContext {
        std::filesystem::path directory;
        std::filesystem::path cache;
        bool offline = false;
    };

    struct PackageQuery {
        std::string text;
        std::optional<std::string> resolver;
        std::optional<std::string> capability;
        std::optional<std::string> tag;
        std::size_t limit = 100;
    };

    struct PackageSummary {
        std::string name;
        Version version;
        std::string provider;
        std::string authority;
        std::string description;
        std::optional<std::string> resolver;
        std::vector<std::string> capabilities;
        std::vector<std::string> tags;
    };

    struct ProviderLayer {
        std::vector<ProviderDefinition> definitions;
        ProviderContext context;
    };

    class PackageProvider {
    public:
        virtual ~PackageProvider() = default;

        [[nodiscard]] virtual ProviderInfo info() const = 0;
        [[nodiscard]] virtual Result<std::vector<PackageCandidate>> candidates(const PackageRequest& request) const = 0;
        [[nodiscard]] virtual Result<std::vector<PackageSummary>> query(const PackageQuery&) const { return std::vector<PackageSummary>{}; }
    };

    struct ProviderDriverInfo {
        std::string name;
        std::string description;
    };

    class ProviderDriver {
    public:
        virtual ~ProviderDriver() = default;

        [[nodiscard]] virtual ProviderDriverInfo info() const = 0;
        [[nodiscard]] virtual Result<std::unique_ptr<PackageProvider>> create(
            const ProviderDefinition& definition,
            const ProviderContext& context
        ) const = 0;
    };

    class ExtensionRegistry;
    [[nodiscard]] Result<void> configure_providers(ExtensionRegistry& registry, const std::vector<ProviderLayer>& layers);
}
