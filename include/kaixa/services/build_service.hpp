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
        const BuildEnvironment& environment
    );
}
