#pragma once

#include <kaixa/config/provider_configuration.hpp>
#include <kaixa/foundation/diagnostic.hpp>
#include <kaixa/model/manifest.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace kaixa {
    struct ProviderInfo {
        std::string name;
        std::string driver;
        bool is_default = false;
    };

    struct PackageCandidate {
        std::string package;
        Version version;
        std::string authority;
        SourceLocator source;
    };

    struct ProviderContext {
        std::filesystem::path directory;
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
