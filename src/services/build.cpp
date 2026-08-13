#include <kaixa/services/build_service.hpp>

namespace kaixa {
    Result<std::vector<BuildProduct>> discover_products(
        const Graph& graph,
        const ResolverRegistry& registry,
        const BuildEnvironment& environment
    ) {
        const PackageNode& root = graph[graph.root()];
        Resolver* resolver = registry.find(root.resolver);
        if (!resolver) {
            return std::unexpected(error(
                "resolver `" + root.resolver + "` is not installed"
            ));
        }

        return resolver->products(graph, root, environment);
    }

    Result<BuildPlan> plan_build(
        const Graph& graph,
        const ResolverRegistry& registry,
        const BuildEnvironment& environment,
        const BuildRequest& request
    ) {
        auto order = graph.build_order();
        if (!order)
            return std::unexpected(order.error());

        BuildPlan plan;
        for (const PackageId id: *order) {
            const PackageNode& package = graph[id];
            if (package.kind == PackageKind::opaque)
                continue;

            Resolver* resolver = registry.find(package.resolver);
            if (!resolver) {
                SourceLocation location;
                if (package.manifest)
                    location = package.manifest->location;
                return std::unexpected(error_at(
                    std::move(location),
                    "resolver `" + package.resolver + "` is not installed"
                ));
            }

            const BuildRequest package_request = id == graph.root()
                ? request
                : BuildRequest{};
            auto planned = resolver->plan(
                graph,
                package,
                environment,
                package_request,
                plan
            );
            if (!planned)
                return std::unexpected(planned.error());
        }
        return plan;
    }

    Result<BuildPlan> plan_tests(
        const Graph& graph,
        const ResolverRegistry& registry,
        const BuildEnvironment& environment,
        const TestRequest& request
    ) {
        auto plan = plan_build(graph, registry, environment);
        if (!plan)
            return std::unexpected(plan.error());

        const PackageNode& root = graph[graph.root()];
        Resolver* resolver = registry.find(root.resolver);
        if (!resolver) {
            return std::unexpected(error(
                "resolver `" + root.resolver + "` is not installed"
            ));
        }

        auto planned = resolver->plan_tests(graph, root, environment, request, *plan);
        if (!planned)
            return std::unexpected(planned.error());

        return plan;
    }
}
