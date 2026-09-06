#include <kaixa/services/workflow_service.hpp>

#include <algorithm>
#include <utility>

namespace kaixa {
    namespace {
        Result<WorkflowDefinition> resolve_workflow(
            const Graph& graph,
            const std::span<const WorkflowDefinition> workflows,
            const std::string_view requested
        ) {
            const std::size_t separator = requested.find(':');
            if (separator != std::string_view::npos) {
                const std::string_view package = requested.substr(0, separator);
                const std::string_view name = requested.substr(separator + 1);
                const auto workflow = std::ranges::find_if(workflows, [&](const WorkflowDefinition& candidate) {
                    return graph[candidate.package].name == package && candidate.declaration.name == name;
                });
                if (workflow == workflows.end())
                    return std::unexpected(error("unknown workflow `" + std::string(requested) + "`"));

                return *workflow;
            }

            const WorkflowDefinition* match = nullptr;
            for (const WorkflowDefinition& workflow: workflows) {
                if (workflow.declaration.name != requested)
                    continue;

                if (match) {
                    return std::unexpected(error("workflow `" + std::string(requested) + "` is provided by multiple selected packages")
                            .add_note("select it as `<package>:" + std::string(requested) + "`"));
                }
                match = &workflow;
            }
            if (!match)
                return std::unexpected(error("unknown workflow `" + std::string(requested) + "`"));

            return *match;
        }

        std::optional<WorkflowStepKind> builtin_kind(const std::string_view name) {
            if (name == "generate")
                return WorkflowStepKind::generate;

            if (name == "build")
                return WorkflowStepKind::build;

            if (name == "test")
                return WorkflowStepKind::test;

            if (name == "bench")
                return WorkflowStepKind::bench;

            return std::nullopt;
        }

        Result<PreparedWorkflowStep> prepare_step(const Graph& graph, const WorkflowDefinition& workflow, const std::string_view declared) {
            if (const auto builtin = builtin_kind(declared))
                return PreparedWorkflowStep{*builtin, std::string(declared), std::nullopt};

            std::string_view task_name = declared;
            if (declared.starts_with("task:")) {
                task_name.remove_prefix(std::string_view("task:").size());
                if (task_name.empty()) {
                    return std::unexpected(error_at(workflow.declaration.location, "workflow step `task:` requires a command name"));
                }
            }

            auto task = prepare_task(graph, task_name, workflow.package);
            if (!task) {
                return std::unexpected(
                    std::move(task).error().add_note(
                        "while resolving step `" + std::string(declared) + "` of workflow `" + workflow.qualified_name + "`"
                    )
                );
            }
            return PreparedWorkflowStep{WorkflowStepKind::task, std::string(declared), std::move(*task)};
        }

        Result<PackageId> automation_package(
            const Graph& graph,
            const std::optional<std::string>& requested,
            const SourceLocation& location,
            const std::string_view kind,
            const std::string_view name
        ) {
            if (!requested) {
                if (graph.roots().size() == 1)
                    return graph.roots().front();

                return std::unexpected(error_at(
                    location,
                    "local " + std::string(kind) + " `" + std::string(name) + "` requires `package` in a multi-root workspace"
                ));
            }

            const auto package = graph.find_by_name(*requested);
            if (!package || !graph.is_root(*package)) {
                return std::unexpected(error_at(
                    location,
                    "local "
                        + std::string(kind)
                        + " `"
                        + std::string(name)
                        + "` selects package `"
                        + *requested
                        + "`, which is not a selected root"
                ));
            }
            return *package;
        }

        Result<Manifest*> automation_manifest(
            Graph& graph,
            const std::optional<std::string>& requested,
            const SourceLocation& location,
            const std::string_view kind,
            const std::string_view name
        ) {
            auto package = automation_package(graph, requested, location, kind, name);
            if (!package)
                return std::unexpected(package.error());

            if (!graph[*package].manifest()) {
                return std::unexpected(
                    error_at(location, "local " + std::string(kind) + " `" + std::string(name) + "` requires a managed package")
                );
            }
            return graph[*package].manifest();
        }
    }

    Result<std::vector<WorkflowDefinition>> discover_workflows(const Graph& graph) {
        std::vector<WorkflowDefinition> result;
        for (const PackageId root: graph.roots()) {
            const PackageNode& package = graph[root];
            if (!package.manifest())
                continue;

            for (const WorkflowDeclaration& declaration: package.manifest()->workflows) {
                result.push_back({root, declaration, package.name + ':' + declaration.name});
            }
        }
        return result;
    }

    Result<void> apply_automation_layers(Graph& graph, std::vector<AutomationDocument> layers) {
        for (AutomationDocument& layer: layers) {
            for (TaskDeclaration& command: layer.commands) {
                auto manifest = automation_manifest(graph, command.package, command.location, "command", command.name);
                if (!manifest)
                    return std::unexpected(manifest.error());

                command.package.reset();
                const auto existing = std::ranges::find((*manifest)->commands, command.name, &TaskDeclaration::name);
                if (existing == (*manifest)->commands.end())
                    (*manifest)->commands.push_back(std::move(command));
                else
                    *existing = std::move(command);
            }

            for (WorkflowDeclaration& workflow: layer.workflows) {
                auto manifest = automation_manifest(graph, workflow.package, workflow.location, "workflow", workflow.name);
                if (!manifest)
                    return std::unexpected(manifest.error());

                workflow.package.reset();
                const auto existing = std::ranges::find((*manifest)->workflows, workflow.name, &WorkflowDeclaration::name);
                if (existing == (*manifest)->workflows.end())
                    (*manifest)->workflows.push_back(std::move(workflow));
                else
                    *existing = std::move(workflow);
            }
        }
        return {};
    }

    Result<WorkflowPreparation> prepare_workflow(const Graph& graph, const std::string_view requested) {
        auto workflows = discover_workflows(graph);
        if (!workflows)
            return std::unexpected(workflows.error());

        auto selected = resolve_workflow(graph, *workflows, requested);
        if (!selected)
            return std::unexpected(selected.error());

        WorkflowPreparation result{std::move(*selected), {}};
        result.steps.reserve(result.workflow.declaration.steps.size());
        for (const std::string& declaration: result.workflow.declaration.steps) {
            auto step = prepare_step(graph, result.workflow, declaration);
            if (!step)
                return std::unexpected(step.error());

            result.steps.push_back(std::move(*step));
        }
        return result;
    }
}
