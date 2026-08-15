#include <kaixa/services/build_service.hpp>

#include <algorithm>
#include <iterator>
#include <utility>
#include <vector>

namespace kaixa {
    Result<std::vector<BuildProduct>> discover_products(
        const Graph& graph,
        const ResolverRegistry& registry,
        const BuildEnvironment& environment
    ) {
        std::vector<BuildProduct> products;
        for (const PackageId id: graph.roots()) {
            const PackageNode& root = graph[id];
            Resolver* resolver = registry.find(root.resolver);
            if (!resolver) {
                return std::unexpected(error(
                    "resolver `" + root.resolver + "` is not installed"
                ));
            }

            auto discovered = resolver->products(graph, root, environment);
            if (!discovered)
                return std::unexpected(discovered.error());

            products.insert(
                products.end(),
                std::make_move_iterator(discovered->begin()),
                std::make_move_iterator(discovered->end())
            );
        }
        return products;
    }

    Result<BuildPlan> plan_build(
        const Graph& graph,
        const ResolverRegistry& registry,
        const BuildEnvironment& environment,
        const BuildRequest& request
    ) {
        if (!request.targets.empty() && !request.packages.empty()) {
            return std::unexpected(error(
                "a build request cannot mix global and package-specific targets"
            ));
        }

        std::vector<PackageId> selected_roots;
        if (request.packages.empty()) {
            selected_roots.assign(graph.roots().begin(), graph.roots().end());
        } else {
            selected_roots.reserve(request.packages.size());
            for (const PackageBuildRequest& package: request.packages) {
                if (!package.build_default && package.targets.empty()) {
                    return std::unexpected(error(
                        "package-specific build request selects neither default nor explicit targets"
                    ));
                }
                if (!graph.is_root(package.package)) {
                    return std::unexpected(error(
                        "package-specific build request selects a package outside the roots"
                    ));
                }
                if (std::ranges::find(selected_roots, package.package) != selected_roots.end()) {
                    return std::unexpected(error(
                        "package-specific build request selects the same package more than once"
                    ));
                }

                selected_roots.push_back(package.package);
            }
        }

        auto order = graph.build_order(selected_roots);
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

            BuildRequest package_request = request;
            package_request.packages.clear();
            if (graph.is_root(id) && !request.packages.empty()) {
                const auto selected = std::ranges::find(
                    request.packages,
                    id,
                    &PackageBuildRequest::package
                );
                package_request.targets = selected->targets;
                package_request.build_default = selected->build_default;
            } else if (!graph.is_root(id)) {
                package_request.targets.clear();
                package_request.build_default = true;
            }

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

        for (const PackageId id: graph.roots()) {
            const PackageNode& root = graph[id];
            Resolver* resolver = registry.find(root.resolver);
            if (!resolver) {
                return std::unexpected(error(
                    "resolver `" + root.resolver + "` is not installed"
                ));
            }

            auto planned = resolver->plan_tests(graph, root, environment, request, *plan);
            if (!planned)
                return std::unexpected(planned.error());
        }

        return plan;
    }
}
