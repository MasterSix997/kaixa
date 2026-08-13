#pragma once

#include <kaixa/build/layout.hpp>
#include <kaixa/build/plan.hpp>
#include <kaixa/extension/registry.hpp>
#include <kaixa/foundation/diagnostic.hpp>
#include <kaixa/model/graph.hpp>

namespace kaixa {
    [[nodiscard]] Result<BuildPlan> plan_build(
        const Graph& graph,
        const ResolverRegistry& registry,
        const BuildEnvironment& environment,
        const BuildRequest& request = {}
    );

    [[nodiscard]] Result<std::vector<BuildProduct>> discover_products(
        const Graph& graph,
        const ResolverRegistry& registry,
        const BuildEnvironment& environment
    );

    [[nodiscard]] Result<BuildPlan> plan_tests(
        const Graph& graph,
        const ResolverRegistry& registry,
        const BuildEnvironment& environment,
        const TestRequest& request
    );
}
