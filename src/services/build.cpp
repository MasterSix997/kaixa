#include <kaixa/services/build_service.hpp>

#include <kaixa/model/effective_product.hpp>

#include <algorithm>
#include <iterator>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace kaixa {
    namespace {
        struct ActiveResolverSession {
            Resolver* resolver = nullptr;
            std::unique_ptr<ResolverSession> session;
        };

        ResolverSession& resolver_session(Resolver& resolver, const Graph& graph, std::vector<ActiveResolverSession>& sessions) {
            const auto existing = std::ranges::find(sessions, &resolver, &ActiveResolverSession::resolver);
            if (existing != sessions.end())
                return *existing->session;

            sessions.push_back({&resolver, resolver.start_session(graph)});
            return *sessions.back().session;
        }

        Result<void> append_build_plan(
            const Graph& graph,
            const ExtensionRegistry& registry,
            const BuildEnvironment& environment,
            const BuildRequest& request,
            const std::span<const ConfiguredPackageInstance> instances,
            std::vector<ActiveResolverSession>& sessions,
            ExecutionPlan& plan
        ) {
            if (!request.targets.empty() && !request.packages.empty()) {
                return std::unexpected(error("a build request cannot mix global and package-specific targets"));
            }

            std::vector<PackageId> selected_roots;
            if (request.packages.empty()) {
                selected_roots.assign(graph.roots().begin(), graph.roots().end());
            } else {
                selected_roots.reserve(request.packages.size());
                for (const PackageBuildRequest& package: request.packages) {
                    if (!package.build_default && package.targets.empty()) {
                        return std::unexpected(error("package-specific build request selects neither default nor explicit targets"));
                    }
                    if (!graph.is_root(package.package)) {
                        return std::unexpected(error("package-specific build request selects a package outside the roots"));
                    }
                    if (std::ranges::find(selected_roots, package.package) != selected_roots.end()) {
                        return std::unexpected(error("package-specific build request selects the same package more than once"));
                    }

                    selected_roots.push_back(package.package);
                }
            }

            auto order = graph.build_order(selected_roots);
            if (!order)
                return std::unexpected(order.error());

            for (const PackageId id: *order) {
                const PackageNode& package = graph[id];
                if (package.is_opaque())
                    continue;

                Resolver* resolver = registry.find_resolver(package.resolver);
                if (!resolver) {
                    SourceLocation location;
                    if (package.manifest())
                        location = package.manifest()->location;

                    return std::unexpected(error_at(std::move(location), "resolver `" + package.resolver + "` is not installed"));
                }

                BuildRequest package_request = request;
                package_request.packages.clear();
                if (graph.is_root(id) && !request.packages.empty()) {
                    const auto selected = std::ranges::find(request.packages, id, &PackageBuildRequest::package);
                    package_request.targets = selected->targets;
                    package_request.build_default = selected->build_default;
                } else if (!graph.is_root(id)) {
                    package_request.targets.clear();
                    package_request.build_default = true;
                }

                auto planned = resolver->plan(
                    graph,
                    registry,
                    package,
                    environment,
                    instances,
                    package_request,
                    plan,
                    resolver_session(*resolver, graph, sessions)
                );
                if (!planned)
                    return std::unexpected(planned.error());
            }
            return {};
        }
    }

    Result<std::vector<BuildProduct>> discover_products(
        const Graph& graph,
        const ExtensionRegistry& registry,
        const BuildEnvironment& environment,
        std::span<const ConfiguredPackageInstance> instances
    ) {
        std::vector<ConfiguredPackageInstance> configured_instances;
        if (instances.empty()) {
            auto configured = configure_package_instances(
                graph,
                {environment.configuration.profile, host_target_os()},
                registry.policy_schema()
            );
            if (!configured)
                return std::unexpected(configured.error());

            configured_instances = std::move(*configured);
            instances = configured_instances;
        }

        std::vector<ActiveResolverSession> sessions;
        std::vector<BuildProduct> products;
        for (const PackageId id: graph.roots()) {
            const PackageNode& root = graph[id];
            Resolver* resolver = registry.find_resolver(root.resolver);
            if (!resolver) {
                return std::unexpected(error("resolver `" + root.resolver + "` is not installed"));
            }

            auto discovered = resolver
                                  ->products(graph, registry, root, environment, instances, resolver_session(*resolver, graph, sessions));
            if (!discovered)
                return std::unexpected(discovered.error());

            products.insert(products.end(), std::make_move_iterator(discovered->begin()), std::make_move_iterator(discovered->end()));
        }
        return products;
    }

    Result<ExecutionPlan> plan_build(
        const Graph& graph,
        const ExtensionRegistry& registry,
        const BuildEnvironment& environment,
        const BuildRequest& request,
        std::span<const ConfiguredPackageInstance> instances
    ) {
        std::vector<ConfiguredPackageInstance> configured_instances;
        if (instances.empty()) {
            auto configured = configure_package_instances(
                graph,
                {environment.configuration.profile, host_target_os()},
                registry.policy_schema()
            );
            if (!configured)
                return std::unexpected(configured.error());

            configured_instances = std::move(*configured);
            instances = configured_instances;
        }

        std::vector<ActiveResolverSession> sessions;
        ExecutionPlan plan;
        auto planned = append_build_plan(graph, registry, environment, request, instances, sessions, plan);
        if (!planned)
            return std::unexpected(planned.error());

        return plan;
    }

    Result<ExecutionPlan> plan_tests(
        const Graph& graph,
        const ExtensionRegistry& registry,
        const BuildEnvironment& environment,
        const TestRequest& request,
        std::span<const ConfiguredPackageInstance> instances
    ) {
        std::vector<ConfiguredPackageInstance> configured_instances;
        if (instances.empty()) {
            auto configured = configure_package_instances(
                graph,
                {environment.configuration.profile, host_target_os()},
                registry.policy_schema()
            );
            if (!configured)
                return std::unexpected(configured.error());

            configured_instances = std::move(*configured);
            instances = configured_instances;
        }

        std::vector<ActiveResolverSession> sessions;
        ExecutionPlan plan;
        auto build = append_build_plan(graph, registry, environment, {}, instances, sessions, plan);
        if (!build)
            return std::unexpected(build.error());

        for (const PackageId id: graph.roots()) {
            const PackageNode& root = graph[id];
            Resolver* resolver = registry.find_resolver(root.resolver);
            if (!resolver) {
                return std::unexpected(error("resolver `" + root.resolver + "` is not installed"));
            }

            auto planned = resolver->plan_tests(
                graph,
                registry,
                root,
                environment,
                instances,
                request,
                plan,
                resolver_session(*resolver, graph, sessions)
            );
            if (!planned)
                return std::unexpected(planned.error());
        }

        return plan;
    }
}
