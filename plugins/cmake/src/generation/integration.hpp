#pragma once

#include <planning/build_context.hpp>

#include <kaixa/foundation/diagnostic.hpp>
#include <kaixa/model/graph.hpp>

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace kaixa::plugin::cmake::detail {
    struct DependencyIntegrationContext {
        const Graph& graph;
        const PackageNode& package;
        const BuildContext& build;
        const std::vector<bool>& normal_source_visited;
        std::span<const PackageId> source_packages;
        const std::vector<std::optional<PreparedProject>>& projects;
    };

    // Emitting the CMake file a configured project includes to see its dependencies. Generation
    // resolves nothing: the routes it needs were already selected and are handed in.
    [[nodiscard]] Result<std::string> dependency_integration(const DependencyIntegrationContext& context);
}
