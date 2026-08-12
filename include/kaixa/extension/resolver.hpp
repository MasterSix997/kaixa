#pragma once

#include <kaixa/build/layout.hpp>
#include <kaixa/build/plan.hpp>
#include <kaixa/foundation/diagnostic.hpp>
#include <kaixa/model/graph.hpp>

#include <optional>
#include <string>

namespace kaixa {
    struct ResolverInfo {
        std::string name;
        std::string description;
    };

    struct TestRequest {
        std::optional<std::string> filter;
        std::optional<std::string> target;
    };

    class Resolver {
    public:
        virtual ~Resolver() = default;

        [[nodiscard]] virtual ResolverInfo info() const = 0;
        [[nodiscard]] virtual Result<void> plan(
            const Graph& graph,
            const PackageNode& package,
            const BuildEnvironment& environment,
            BuildPlan& plan
        ) const = 0;

        [[nodiscard]] virtual Result<void> plan_tests(
            const Graph& graph,
            const PackageNode& package,
            const BuildEnvironment& environment,
            const TestRequest& request,
            BuildPlan& plan
        ) const = 0;
    };
}
