#pragma once

#include <kaixa/extension/provider.hpp>
#include <kaixa/extension/resolver.hpp>
#include <kaixa/extension/source.hpp>
#include <kaixa/test/adapter.hpp>

#include <algorithm>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace kaixa {
    class ExtensionRegistry {
    public:
        [[nodiscard]] Result<void> add(std::unique_ptr<Resolver> resolver) {
            auto registered = m_policy_schema.add(resolver->policies());
            if (!registered)
                return std::unexpected(std::move(registered).error().add_note("registered by resolver `" + resolver->info().name + "`"));

            m_resolvers.push_back(std::move(resolver));
            return {};
        }

        [[nodiscard]] const PolicySchema& policy_schema() const noexcept { return m_policy_schema; }

        void add(std::unique_ptr<SourceDriver> driver) { m_source_drivers.push_back(std::move(driver)); }

        void add(std::unique_ptr<PackageProvider> provider) { m_providers.push_back(std::move(provider)); }

        void add(std::unique_ptr<ProviderDriver> driver) { m_provider_drivers.push_back(std::move(driver)); }

        void add(TestAdapterInfo adapter) { m_test_adapters.push_back(std::move(adapter)); }

        [[nodiscard]] Resolver* find_resolver(std::string_view name) const {
            for (const auto& resolver: m_resolvers) {
                if (resolver->info().name == name)
                    return resolver.get();
            }
            return nullptr;
        }

        [[nodiscard]] SourceDriver* find_source_driver(std::string_view name) const {
            for (const auto& driver: m_source_drivers) {
                if (driver->info().name == name)
                    return driver.get();
            }
            return nullptr;
        }

        [[nodiscard]] PackageProvider* find_provider(std::string_view name) const {
            for (const auto& provider: m_providers) {
                if (provider->info().name == name)
                    return provider.get();
            }
            return nullptr;
        }

        [[nodiscard]] ProviderDriver* find_provider_driver(std::string_view name) const {
            for (const auto& driver: m_provider_drivers) {
                if (driver->info().name == name)
                    return driver.get();
            }
            return nullptr;
        }

        [[nodiscard]] const TestAdapterInfo* find_test_adapter(std::string_view name) const noexcept {
            const auto adapter = std::ranges::find(m_test_adapters, name, &TestAdapterInfo::name);
            return adapter == m_test_adapters.end() ? nullptr : &*adapter;
        }

        Result<void> configure_provider(const ProviderDefinition& definition, const ProviderContext& context) {
            if (!is_valid_identifier(definition.name))
                return std::unexpected(error_at(definition.location, "invalid provider name `" + definition.name + "`"));

            if (!is_valid_identifier(definition.driver))
                return std::unexpected(error_at(definition.location, "invalid provider driver name `" + definition.driver + "`"));

            ProviderDriver* driver = find_provider_driver(definition.driver);
            if (!driver) {
                return std::unexpected(error_at(definition.location, "provider driver `" + definition.driver + "` is not installed"));
            }
            if (find_provider(definition.name)) {
                return std::unexpected(error_at(definition.location, "provider `" + definition.name + "` is already configured"));
            }

            auto provider = driver->create(definition, context);
            if (!provider)
                return std::unexpected(provider.error());

            const ProviderInfo configured = (*provider)->info();
            if (configured.name != definition.name
                || configured.driver != definition.driver
                || configured.is_default != definition.is_default) {
                return std::unexpected(
                    error_at(definition.location, "provider driver `" + definition.driver + "` returned inconsistent metadata")
                );
            }

            m_providers.push_back(std::move(*provider));
            return {};
        }

        [[nodiscard]] std::span<const std::unique_ptr<Resolver>> resolvers() const noexcept { return m_resolvers; }

        [[nodiscard]] std::span<const std::unique_ptr<SourceDriver>> source_drivers() const noexcept { return m_source_drivers; }

        [[nodiscard]] std::span<const std::unique_ptr<PackageProvider>> providers() const noexcept { return m_providers; }

        [[nodiscard]] std::span<const std::unique_ptr<ProviderDriver>> provider_drivers() const noexcept { return m_provider_drivers; }

        [[nodiscard]] std::span<const TestAdapterInfo> test_adapters() const noexcept { return m_test_adapters; }

    private:
        PolicySchema m_policy_schema = core_policy_schema();
        std::vector<std::unique_ptr<Resolver>> m_resolvers;
        std::vector<std::unique_ptr<SourceDriver>> m_source_drivers;
        std::vector<std::unique_ptr<PackageProvider>> m_providers;
        std::vector<std::unique_ptr<ProviderDriver>> m_provider_drivers;
        std::vector<TestAdapterInfo> m_test_adapters;
    };

}
