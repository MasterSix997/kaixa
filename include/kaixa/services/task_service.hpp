#pragma once

#include <kaixa/build/product.hpp>
#include <kaixa/extension/registry.hpp>
#include <kaixa/extension/resolver.hpp>
#include <kaixa/model/graph.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kaixa {
    struct TaskDefinition {
        PackageId package;
        TaskDeclaration declaration;
        std::optional<std::string> associated_target;
        std::string qualified_name;
    };

    struct TaskPreparation {
        std::vector<TaskDefinition> tasks;
        BuildRequest build;
        std::string selected;
        bool requires_products = false;
    };

    [[nodiscard]] Result<std::vector<TaskDefinition>> discover_tasks(const Graph& graph);
    [[nodiscard]] Result<TaskPreparation> prepare_task(
        const Graph& graph,
        std::string_view requested,
        std::optional<PackageId> relative_package = std::nullopt
    );
    [[nodiscard]] Result<ExecutionPlan> plan_task(
        const Graph& graph,
        const ExtensionRegistry& registry,
        const BuildEnvironment& environment,
        const TaskPreparation& preparation,
        std::span<const BuildProduct> products = {},
        std::span<const std::string> arguments = {},
        std::span<const ConfiguredPackageInstance> instances = {}
    );
}
