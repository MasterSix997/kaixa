#pragma once

#include <model/project_model.hpp>

#include <kaixa/foundation/diagnostic.hpp>
#include <kaixa/model/effective_product.hpp>
#include <kaixa/model/graph.hpp>

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kaixa::plugin::cmake::detail {
    // Assembly: composes the schema and the model phases into one `Options` per configured
    // package, and remembers the result. It is the only phase that sees a package's raw
    // resolver options.
    struct ConfigurationCacheKey {
        PackageId package;
        std::string configured_context;
        std::optional<std::string> policy;

        [[nodiscard]] bool operator==(const ConfigurationCacheKey&) const = default;
        [[nodiscard]] auto operator<=>(const ConfigurationCacheKey&) const = default;
    };

    struct ConfigurationCache {
        explicit ConfigurationCache(const std::size_t package_count = 0)
            : effective_packages(package_count) {}

        std::vector<std::optional<EffectivePackage>> effective_packages;
        std::map<ConfigurationCacheKey, Options> options;
        std::optional<std::vector<bool>> install_requirements;
        FileCatalog files;
    };

    [[nodiscard]] Result<const Options*> read_cached_options(
        const Graph& graph,
        const ExtensionRegistry& registry,
        const PackageNode& package,
        const ProductRealizationContext& realization,
        const EffectivePolicy* policy_override,
        std::string_view configured_context,
        ConfigurationCache& cache
    );
}
