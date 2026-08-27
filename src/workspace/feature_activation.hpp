#pragma once

#include <kaixa/model/graph.hpp>

#include <functional>
#include <span>

namespace kaixa::workspace_detail {
    using FeatureDependencyResolver = std::function<Result<PackageId>(PackageId, const DependencyBinding&)>;
    using FeatureMemberResolver = std::function<Result<PackageId>(PackageId, std::string_view, const SourceLocation&)>;

    class FeatureActivator {
    public:
        FeatureActivator(Graph& graph, FeatureDependencyResolver dependency_resolver, FeatureMemberResolver member_resolver)
            : m_graph(graph)
            , m_dependency_resolver(std::move(dependency_resolver))
            , m_member_resolver(std::move(member_resolver)) {}

        [[nodiscard]] Result<void> activate(PackageId package, std::span<const std::string> requested, const SourceLocation& location);

    private:
        [[nodiscard]] Result<PackageId> add_dependency(PackageId package, const DependencyBinding& dependency);
        [[nodiscard]] Result<void> activate_dependencies(
            PackageId package,
            std::span<const DependencyBinding> dependencies,
            const FeatureDefinition& feature
        );
        [[nodiscard]] Result<void> activate_members(PackageId package, const FeatureDefinition& feature);
        [[nodiscard]] Result<void> activate_legacy(
            PackageId package,
            std::span<const DependencyBinding> dependencies,
            std::span<const FeatureDefinition> features,
            const FeatureDefinition& feature
        );

        Graph& m_graph;
        FeatureDependencyResolver m_dependency_resolver;
        FeatureMemberResolver m_member_resolver;
    };
}
