#include "feature_activation.hpp"

#include <algorithm>
#include <array>

namespace kaixa::workspace_detail {
    namespace {
        const DependencyBinding* find_feature_dependency(
            const std::span<const DependencyBinding> dependencies,
            const std::string_view name
        ) {
            const auto binding = std::ranges::find_if(dependencies, [&](const DependencyBinding& candidate) {
                return candidate.local_name() == name || candidate.request.package == name;
            });
            return binding == dependencies.end() ? nullptr : &*binding;
        }
    }

    Result<PackageId> FeatureActivator::add_dependency(const PackageId package, const DependencyBinding& dependency) {
        auto resolved = m_dependency_resolver(package, dependency);
        if (!resolved)
            return std::unexpected(resolved.error());

        PackageNode& node = m_graph[package];
        if (std::ranges::find(node.dependencies, *resolved) == node.dependencies.end())
            node.dependencies.push_back(*resolved);

        return *resolved;
    }

    Result<void> FeatureActivator::activate_dependencies(
        const PackageId package,
        const std::span<const DependencyBinding> dependencies,
        const FeatureDefinition& feature
    ) {
        for (const std::string& dependency_name: feature.dependencies) {
            const DependencyBinding* dependency = find_feature_dependency(dependencies, dependency_name);
            if (!dependency) {
                return std::unexpected(
                    error_at(feature.location, "feature `" + feature.name + "` activates unknown dependency `" + dependency_name + "`")
                );
            }
            auto resolved = add_dependency(package, *dependency);
            if (!resolved)
                return std::unexpected(resolved.error());
        }

        for (const auto& [dependency_name, features]: feature.dependency_features) {
            const DependencyBinding* dependency = find_feature_dependency(dependencies, dependency_name);
            if (!dependency) {
                return std::unexpected(
                    error_at(feature.location, "feature `" + feature.name + "` configures unknown dependency `" + dependency_name + "`")
                );
            }
            DependencyBinding configured = *dependency;
            configured.request.features.insert(configured.request.features.end(), features.begin(), features.end());
            auto resolved = add_dependency(package, configured);
            if (!resolved)
                return std::unexpected(resolved.error());
        }
        return {};
    }

    Result<void> FeatureActivator::activate_members(const PackageId package, const FeatureDefinition& feature) {
        for (const std::string& member_name: feature.members) {
            auto resolved = m_member_resolver(package, member_name, feature.location);
            if (!resolved)
                return std::unexpected(resolved.error());

            PackageNode& node = m_graph[package];
            if (std::ranges::find(node.dependencies, *resolved) == node.dependencies.end())
                node.dependencies.push_back(*resolved);
        }
        return {};
    }

    Result<void> FeatureActivator::activate_legacy(
        const PackageId package,
        const std::span<const DependencyBinding> dependencies,
        const std::span<const FeatureDefinition> features,
        const FeatureDefinition& feature
    ) {
        for (const std::string& activation: feature.features) {
            const std::size_t separator = activation.find('/');
            if (separator == std::string::npos) {
                const DependencyBinding* dependency = find_feature_dependency(dependencies, activation);
                if (dependency && dependency->request.optional) {
                    auto resolved = add_dependency(package, *dependency);
                    if (!resolved)
                        return std::unexpected(resolved.error());
                } else if (std::ranges::find(features, activation, &FeatureDefinition::name) != features.end()) {
                    const std::array<std::string, 1> local_name{activation};
                    auto local = activate(package, local_name, feature.location);
                    if (!local)
                        return std::unexpected(local.error());
                }
                continue;
            }

            const std::string dependency_name = activation.substr(0, separator);
            const DependencyBinding* dependency = find_feature_dependency(dependencies, dependency_name);
            if (!dependency)
                continue;

            DependencyBinding configured = *dependency;
            configured.request.features.push_back(activation.substr(separator + 1));
            auto resolved = add_dependency(package, configured);
            if (!resolved)
                return std::unexpected(resolved.error());
        }
        return {};
    }

    Result<void> FeatureActivator::activate(
        const PackageId package,
        const std::span<const std::string> requested,
        const SourceLocation& location
    ) {
        if (requested.empty())
            return {};

        if (!m_graph[package].manifest()) {
            for (const std::string& name: requested) {
                if (std::ranges::find(m_graph[package].active_features, name) == m_graph[package].active_features.end())
                    m_graph[package].active_features.push_back(name);
            }
            return {};
        }

        const std::vector<DependencyBinding> dependencies = m_graph[package].manifest()->dependencies;
        const std::vector<FeatureDefinition> features = m_graph[package].manifest()->features;
        for (const std::string& name: requested) {
            if (std::ranges::find(m_graph[package].active_features, name) != m_graph[package].active_features.end())
                continue;

            const auto definition = std::ranges::find(features, name, &FeatureDefinition::name);
            if (definition == features.end()) {
                return std::unexpected(error_at(location, "package `" + m_graph[package].name + "` has no feature `" + name + "`"));
            }
            m_graph[package].active_features.push_back(name);

            if (!definition->legacy) {
                auto local = activate(package, definition->features, definition->location);
                if (!local)
                    return std::unexpected(local.error());
            }

            auto activated_dependencies = activate_dependencies(package, dependencies, *definition);
            if (!activated_dependencies)
                return std::unexpected(activated_dependencies.error());

            auto members = activate_members(package, *definition);
            if (!members)
                return std::unexpected(members.error());

            if (definition->legacy) {
                auto legacy = activate_legacy(package, dependencies, features, *definition);
                if (!legacy)
                    return std::unexpected(legacy.error());
            }
        }
        return {};
    }
}
