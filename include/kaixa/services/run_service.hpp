#pragma once

#include <kaixa/build/layout.hpp>
#include <kaixa/build/plan.hpp>
#include <kaixa/extension/registry.hpp>
#include <kaixa/extension/resolver.hpp>
#include <kaixa/foundation/diagnostic.hpp>
#include <kaixa/model/graph.hpp>

#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace kaixa {
    [[nodiscard]] Result<std::vector<RunTarget>> discover_executable_targets(
        const Graph& graph,
        const ResolverRegistry& registry,
        const BuildEnvironment& environment
    );

    [[nodiscard]] Result<std::vector<RunTarget>> discover_run_targets(
        const Graph& graph,
        const ResolverRegistry& registry,
        const BuildEnvironment& environment
    );

    [[nodiscard]] Result<RunTarget> select_run_target(
        std::span<const RunTarget> targets,
        const std::optional<std::string>& requested,
        std::string_view package_name
    );

    [[nodiscard]] Result<BuildPlan> plan_run(
        const Graph& graph,
        const ResolverRegistry& registry,
        const BuildEnvironment& environment,
        std::string target
    );
}
