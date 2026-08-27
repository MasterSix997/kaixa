#pragma once

#include <kaixa/build/layout.hpp>
#include <kaixa/build/plan.hpp>
#include <kaixa/extension/registry.hpp>
#include <kaixa/foundation/diagnostic.hpp>
#include <kaixa/model/graph.hpp>

namespace kaixa {
    [[nodiscard]] Result<BuildPlan> plan_build(
        const Graph& graph,
        const ExtensionRegistry& registry,
        const BuildEnvironment& environment,
        const BuildRequest& request = {},
        std::span<const ConfiguredPackageInstance> instances = {}
    );

    [[nodiscard]] Result<std::vector<BuildProduct>> discover_products(
        const Graph& graph,
        const ExtensionRegistry& registry,
        const BuildEnvironment& environment,
        std::span<const ConfiguredPackageInstance> instances = {}
    );

    [[nodiscard]] Result<BuildPlan> plan_tests(
        const Graph& graph,
        const ExtensionRegistry& registry,
        const BuildEnvironment& environment,
        const TestRequest& request,
        std::span<const ConfiguredPackageInstance> instances = {}
    );
}
