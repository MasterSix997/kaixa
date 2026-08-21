#include <kaixa/extension/provider.hpp>

#include <kaixa/extension/registry.hpp>

#include <algorithm>

namespace kaixa {
    Result<void> configure_providers(ExtensionRegistry& registry, const std::vector<ProviderLayer>& layers) {
        struct SelectedProvider {
            ProviderDefinition definition;
            ProviderContext context;
        };

        std::vector<SelectedProvider> selected;
        for (const ProviderLayer& layer: layers) {
            for (const ProviderDefinition& definition: layer.definitions) {
                const auto existing = std::ranges::find_if(selected, [&](const SelectedProvider& provider) {
                    return provider.definition.name == definition.name;
                });
                SelectedProvider provider{
                    definition, definition.directory.empty() ? layer.context : ProviderContext{definition.directory}
                };
                if (existing == selected.end())
                    selected.push_back(std::move(provider));
                else
                    *existing = std::move(provider);
            }
        }

        std::size_t default_count = 0;
        for (const auto& provider: registry.providers()) {
            if (provider->info().is_default)
                ++default_count;
        }

        for (const SelectedProvider& provider: selected) {
            if (provider.definition.is_default)
                ++default_count;
        }

        if (default_count > 1)
            return std::unexpected(error("more than one default package provider is configured"));

        for (const SelectedProvider& provider: selected) {
            auto configured = registry.configure_provider(provider.definition, provider.context);
            if (!configured)
                return std::unexpected(configured.error());
        }

        return {};
    }
}
