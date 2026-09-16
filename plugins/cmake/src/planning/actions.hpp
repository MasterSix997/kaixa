#pragma once

#include <planning/build_context.hpp>

#include <kaixa/build/action.hpp>
#include <kaixa/build/plan.hpp>
#include <kaixa/extension/resolver.hpp>
#include <kaixa/model/graph.hpp>

#include <filesystem>
#include <optional>
#include <vector>

namespace kaixa::plugin::cmake::detail {
    struct ConfigureActionContext {
        const Graph& graph;
        const PackageNode& package;
        const BuildContext& build;
        const ConfiguredPackageInstance& instance;
        const std::optional<std::filesystem::path>& install;
        const std::filesystem::path& integration_file;
        const std::vector<std::filesystem::path>& prefixes;
        const std::vector<PackageId>& source_packages;
        const std::vector<std::optional<PreparedProject>>& projects;
        bool reset = false;
    };

    // Building the CMake commands for one configured route. These read the build context and
    // nothing else; the caller decides which execution phase each action belongs to.
    [[nodiscard]] Result<Action> configure_action(const ConfigureActionContext& route);
    [[nodiscard]] Action build_action(
        const PackageNode& package,
        const BuildContext& context,
        const ConfiguredPackageInstance& instance,
        const BuildRequest& request,
        bool selected
    );
    [[nodiscard]] Action install_action(
        const PackageNode& package,
        const BuildContext& context,
        const ConfiguredPackageInstance& instance,
        const std::filesystem::path& destination
    );
    void append_build_action(ExecutionPlan& plan, Action action, bool installing);
}
