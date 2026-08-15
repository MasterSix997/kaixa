#include <kaixa/services/run_service.hpp>

#include <kaixa/services/build_service.hpp>

#include <algorithm>
#include <string>
#include <utility>

namespace kaixa {
    Result<std::vector<RunTarget>> discover_executable_targets(
        const Graph& graph,
        const ResolverRegistry& registry,
        const BuildEnvironment& environment
    ) {
        auto products = discover_products(graph, registry, environment);
        if (!products)
            return std::unexpected(products.error());

        std::vector<RunTarget> targets;
        for (const BuildProduct& product: *products) {
            if (product.kind != ProductKind::executable || !product.artifact)
                continue;

            targets.push_back({
                product.name,
                product.purpose,
                ProcessRequest{{product.artifact->string()}, graph[product.package].directory}
            });
        }
        return targets;
    }

    Result<std::vector<RunTarget>> discover_run_targets(
        const Graph& graph,
        const ResolverRegistry& registry,
        const BuildEnvironment& environment
    ) {
        auto targets = discover_executable_targets(graph, registry, environment);
        if (!targets)
            return std::unexpected(targets.error());

        std::erase_if(*targets, [](const RunTarget& target) {
            return target.purpose == ProductPurpose::test
                || target.purpose == ProductPurpose::benchmark;
        });
        return targets;
    }

    Result<RunTarget> select_run_target(
        const std::span<const RunTarget> targets,
        const std::optional<std::string>& requested,
        const std::string_view package_name
    ) {
        if (requested) {
            const std::size_t matches = static_cast<std::size_t>(std::ranges::count(
                targets,
                *requested,
                &RunTarget::name
            ));
            if (matches > 1) {
                return std::unexpected(error(
                    "runnable target `" + *requested
                        + "` is provided by multiple selected packages"
                ).add_note("narrow the operation with `--package <name>`"));
            }

            const auto selected = std::ranges::find_if(
                targets,
                [&](const RunTarget& target) { return target.name == *requested; }
            );
            if (selected == targets.end()) {
                if (targets.empty()) {
                    return std::unexpected(error(
                        "runnable target `" + *requested + "` does not exist"
                    ));
                }

                std::string available;
                for (const RunTarget& target: targets) {
                    if (!available.empty())
                        available += ", ";

                    available += target.name;
                }
                return std::unexpected(error(
                    "runnable target `" + *requested + "` does not exist"
                ).add_note("available targets: " + available));
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
        request.targets.push_back(std::move(target));
        request.build_default = false;
        return plan_build(graph, registry, environment, request);
    }
}
