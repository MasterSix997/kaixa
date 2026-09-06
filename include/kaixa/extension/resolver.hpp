#pragma once

#include <kaixa/build/layout.hpp>
#include <kaixa/build/plan.hpp>
#include <kaixa/build/product.hpp>
#include <kaixa/clean/plan.hpp>
#include <kaixa/foundation/diagnostic.hpp>
#include <kaixa/foundation/process.hpp>
#include <kaixa/model/graph.hpp>
#include <kaixa/model/policy.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace kaixa {
    class ExtensionRegistry;

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
        ProductPurpose purpose = ProductPurpose::test;
    };

    struct PackageBuildRequest {
        PackageId package;
        std::vector<std::string> targets;
        bool build_default = false;
    };

    struct BuildRequest {
        std::vector<std::string> targets;
        std::optional<std::size_t> jobs;
        bool build_default = true;
        std::vector<PackageBuildRequest> packages;
        bool install = false;
        std::optional<std::filesystem::path> install_prefix;
    };

    struct RunTarget {
        std::string name;
        ProductPurpose purpose = ProductPurpose::primary;
        ProcessRequest process;
        std::optional<PackageId> package;
    };

    struct CleanRequest {
        bool generated_files = false;
    };

    class ResolverSession {
    public:
        virtual ~ResolverSession() = default;
    };

    class Resolver {
    public:
        virtual ~Resolver() = default;

        [[nodiscard]] virtual ResolverInfo info() const = 0;
        [[nodiscard]] virtual std::span<const PolicyDefinition> policies() const { return {}; }
        [[nodiscard]] virtual std::unique_ptr<ResolverSession> start_session(const Graph& graph) const = 0;
        [[nodiscard]] virtual Result<void> plan(
            const Graph& graph,
            const ExtensionRegistry& registry,
            const PackageNode& package,
            const BuildEnvironment& environment,
            std::span<const ConfiguredPackageInstance> instances,
            const BuildRequest& request,
            ExecutionPlan& plan,
            ResolverSession& session
        ) const = 0;

        [[nodiscard]] virtual Result<void> plan_tests(
            const Graph& graph,
            const ExtensionRegistry& registry,
            const PackageNode& package,
            const BuildEnvironment& environment,
            std::span<const ConfiguredPackageInstance> instances,
            const TestRequest& request,
            ExecutionPlan& plan,
            ResolverSession& session
        ) const = 0;

        [[nodiscard]] virtual Result<std::vector<BuildProduct>> products(
            const Graph& graph,
            const ExtensionRegistry& registry,
            const PackageNode& package,
            const BuildEnvironment& environment,
            std::span<const ConfiguredPackageInstance> instances,
            ResolverSession& session
        ) const = 0;

        [[nodiscard]] virtual Result<void> plan_clean(
            const Graph& graph,
            const ExtensionRegistry& registry,
            const PackageNode& package,
            const BuildEnvironment& environment,
            std::span<const ConfiguredPackageInstance> instances,
            const CleanRequest& request,
            CleanPlan& plan,
            ResolverSession& session
        ) const = 0;
    };
}
