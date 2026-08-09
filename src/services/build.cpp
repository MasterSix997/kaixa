#include <kaixa/services/build_service.hpp>

namespace kaixa {
    Result<BuildPlan> plan_build(
        const Graph& graph,
        const ResolverRegistry& registry,
        const BuildEnvironment& environment
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

            auto planned = resolver->plan(graph, package, environment, plan);
            if (!planned)
                return std::unexpected(planned.error());
        }
        return plan;
    }
}
