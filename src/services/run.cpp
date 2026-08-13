#include <kaixa/services/run_service.hpp>

#include <kaixa/services/build_service.hpp>

#include <algorithm>
#include <string>
#include <utility>

namespace kaixa {
    Result<std::vector<RunTarget>> discover_run_targets(
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

        return resolver->run_targets(graph, root, environment);
    }

    Result<RunTarget> select_run_target(
        const std::span<const RunTarget> targets,
        const std::optional<std::string>& requested,
        const std::string_view package_name
    ) {
        if (requested) {
            const auto selected = std::ranges::find_if(
                targets,
                [&](const RunTarget& target) { return target.name == *requested; }
            );
            if (selected == targets.end()) {
                return std::unexpected(error(
                    "runnable target `" + *requested + "` does not exist"
                ));
            }

            return *selected;
        }

        const auto named_after_package = std::ranges::find_if(
            targets,
            [&](const RunTarget& target) { return target.name == package_name; }
        );
        if (named_after_package != targets.end())
            return *named_after_package;

        if (targets.size() == 1)
            return targets.front();

        if (targets.empty())
            return std::unexpected(error("package has no runnable targets"));

        std::string choices;
        for (const RunTarget& target: targets) {
            if (!choices.empty())
                choices += ", ";

            choices += target.name;
        }
        return std::unexpected(error(
            "multiple runnable targets are available"
        ).add_note("select one with `--target`: " + choices));
    }

    Result<BuildPlan> plan_run(
        const Graph& graph,
        const ResolverRegistry& registry,
        const BuildEnvironment& environment,
        std::string target
    ) {
        BuildRequest request;
        request.target = std::move(target);
        return plan_build(graph, registry, environment, request);
    }
}
