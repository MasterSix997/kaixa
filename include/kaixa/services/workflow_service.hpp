#pragma once

#include <kaixa/model/graph.hpp>
#include <kaixa/services/task_service.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kaixa {
    enum class WorkflowStepKind {
        generate,
        build,
        test,
        bench,
        task
    };

    struct WorkflowDefinition {
        PackageId package;
        WorkflowDeclaration declaration;
        std::string qualified_name;
    };

    struct PreparedWorkflowStep {
        WorkflowStepKind kind = WorkflowStepKind::task;
        std::string name;
        std::optional<TaskPreparation> task;
    };

    struct WorkflowPreparation {
        WorkflowDefinition workflow;
        std::vector<PreparedWorkflowStep> steps;
    };

    [[nodiscard]] Result<std::vector<WorkflowDefinition>> discover_workflows(const Graph& graph);
    [[nodiscard]] Result<void> apply_automation_layers(Graph& graph, std::vector<AutomationDocument> layers);
    [[nodiscard]] Result<WorkflowPreparation> prepare_workflow(const Graph& graph, std::string_view requested);
}
