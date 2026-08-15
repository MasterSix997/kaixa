#pragma once

#include <kaixa/extension/provider.hpp>
#include <kaixa/extension/resolver.hpp>
#include <kaixa/extension/source.hpp>

#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace kaixa {
    class ExtensionRegistry {
    public:
        void add(std::unique_ptr<Resolver> resolver) {
            m_resolvers.push_back(std::move(resolver));
        }

        void add(std::unique_ptr<SourceDriver> driver) {
            m_source_drivers.push_back(std::move(driver));
        }

        void add(std::unique_ptr<PackageProvider> provider) {
            m_providers.push_back(std::move(provider));
        }

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

        [[nodiscard]] std::span<const std::unique_ptr<Resolver>> resolvers() const noexcept {
            return m_resolvers;
        }

        [[nodiscard]] std::span<const std::unique_ptr<SourceDriver>> source_drivers() const noexcept {
            return m_source_drivers;
        }

        [[nodiscard]] std::span<const std::unique_ptr<PackageProvider>> providers() const noexcept {
            return m_providers;
        }

    private:
        std::vector<std::unique_ptr<Resolver>> m_resolvers;
        std::vector<std::unique_ptr<SourceDriver>> m_source_drivers;
        std::vector<std::unique_ptr<PackageProvider>> m_providers;
    };

}
