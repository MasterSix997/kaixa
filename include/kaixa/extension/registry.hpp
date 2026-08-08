#pragma once

#include <kaixa/extension/resolver.hpp>

#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace kaixa {
    class ResolverRegistry {
    public:
        void add(std::unique_ptr<Resolver> resolver) {
            m_resolvers.push_back(std::move(resolver));
        }

        [[nodiscard]] Resolver* find(std::string_view name) const {
            for (const auto& resolver: m_resolvers) {
                if (resolver->info().name == name)
                    return resolver.get();
            }
            return nullptr;
        }

        [[nodiscard]] std::span<const std::unique_ptr<Resolver>> resolvers() const noexcept {
            return m_resolvers;
        }

    private:
        std::vector<std::unique_ptr<Resolver>> m_resolvers;
    };
}
