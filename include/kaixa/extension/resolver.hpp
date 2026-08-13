#pragma once

#include <kaixa/build/layout.hpp>
#include <kaixa/build/plan.hpp>
#include <kaixa/build/product.hpp>
#include <kaixa/clean/plan.hpp>
#include <kaixa/foundation/diagnostic.hpp>
#include <kaixa/foundation/process.hpp>
#include <kaixa/model/graph.hpp>

#include <optional>
#include <string>
#include <vector>

namespace kaixa {
    struct ResolverInfo {
        std::string name;
        std::string description;
    };

    enum class TestMode {
        run,
        list
    };

    struct TestRequest {
        std::optional<std::string> filter;
        std::optional<std::string> target;
        TestMode mode = TestMode::run;
    };

    struct BuildRequest {
        std::vector<std::string> targets;
    };

    struct RunTarget {
        std::string name;
        ProcessRequest process;
    };

    struct CleanRequest {
        bool generated_files = false;
    };

    class Resolver {
    public:
        virtual ~Resolver() = default;

        [[nodiscard]] virtual ResolverInfo info() const = 0;
        [[nodiscard]] virtual Result<void> plan(
            const Graph& graph,
            const PackageNode& package,
            const BuildEnvironment& environment,
            const BuildRequest& request,
            BuildPlan& plan
        ) const = 0;

        [[nodiscard]] virtual Result<void> plan_tests(
            const Graph& graph,
            const PackageNode& package,
            const BuildEnvironment& environment,
            const TestRequest& request,
            BuildPlan& plan
        ) const = 0;

        [[nodiscard]] virtual Result<std::vector<BuildProduct>> products(
            const Graph& graph,
            const PackageNode& package,
            const BuildEnvironment& environment
        ) const = 0;

        [[nodiscard]] virtual Result<void> plan_clean(
            const Graph& graph,
            const PackageNode& package,
            const BuildEnvironment& environment,
            const CleanRequest& request,
            CleanPlan& plan
        ) const = 0;
    };
}
