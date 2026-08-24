#include <kaixa/services/task_service.hpp>

#include <kaixa/model/effective_product.hpp>
#include <kaixa/model/file_set.hpp>
#include <kaixa/services/build_service.hpp>

#include <algorithm>
#include <functional>
#include <map>
#include <utility>

namespace kaixa {
    namespace {
        struct TaskAnalysis {
            std::vector<TaskDefinition> available;
            std::vector<std::size_t> ordered;
            std::vector<int> states;
            BuildRequest build;
            bool requires_products = false;
        };

        Result<void> append_task(
            std::vector<TaskDefinition>& tasks,
            const Graph& graph,
            const PackageNode& package,
            const TaskDeclaration& declaration,
            std::optional<std::string> associated_target
        ) {
            const auto duplicate = std::ranges::find_if(tasks, [&](const TaskDefinition& task) {
                return task.package == package.id && task.declaration.name == declaration.name;
            });
            if (duplicate != tasks.end()) {
                return std::unexpected(error_at(
                    declaration.location,
                    "command `" + declaration.name + "` is declared more than once for package `" + package.name + "`"
                ));
            }

            tasks.push_back({package.id, declaration, std::move(associated_target), package.name + ':' + declaration.name});
            return {};
        }

        Result<std::size_t> resolve_task(
            const std::span<const TaskDefinition> tasks,
            const Graph& graph,
            const std::string_view reference,
            const std::optional<PackageId> relative_package
        ) {
            const std::size_t separator = reference.find(':');
            if (separator != std::string_view::npos) {
                const std::string_view package_name = reference.substr(0, separator);
                const std::string_view task_name = reference.substr(separator + 1);
                const auto task = std::ranges::find_if(tasks, [&](const TaskDefinition& candidate) {
                    return graph[candidate.package].name == package_name && candidate.declaration.name == task_name;
                });
                if (task == tasks.end())
                    return std::unexpected(error("unknown command `" + std::string(reference) + "`"));

                return static_cast<std::size_t>(task - tasks.begin());
            }

            if (relative_package) {
                const auto local = std::ranges::find_if(tasks, [&](const TaskDefinition& candidate) {
                    return candidate.package == *relative_package && candidate.declaration.name == reference;
                });
                if (local != tasks.end())
                    return static_cast<std::size_t>(local - tasks.begin());
            }

            std::optional<std::size_t> match;
            for (std::size_t index = 0; index < tasks.size(); ++index) {
                if (tasks[index].declaration.name != reference)
                    continue;

                if (match) {
                    return std::unexpected(error("command `" + std::string(reference) + "` is provided by multiple selected packages")
                            .add_note("select it as `<package>:" + std::string(reference) + "`"));
                }
                match = index;
            }
            if (!match)
                return std::unexpected(error("unknown command `" + std::string(reference) + "`"));

            return *match;
        }

        PackageBuildRequest& build_requirement(BuildRequest& request, const PackageId package) {
            const auto existing = std::ranges::find(request.packages, package, &PackageBuildRequest::package);
            if (existing != request.packages.end())
                return *existing;

            request.packages.push_back({package, {}, false});
            return request.packages.back();
        }

        void require_default_build(BuildRequest& request, const PackageId package) {
            build_requirement(request, package).build_default = true;
        }

        void require_target_build(BuildRequest& request, const PackageId package, const std::string_view target) {
            PackageBuildRequest& requirement = build_requirement(request, package);
            if (std::ranges::find(requirement.targets, target) == requirement.targets.end())
                requirement.targets.emplace_back(target);
        }

        Result<void> analyze_placeholders(TaskAnalysis& analysis, const TaskDefinition& task, const std::string_view value) {
            std::size_t position = 0;
            while ((position = value.find("${", position)) != std::string_view::npos) {
                const std::size_t end = value.find('}', position + 2);
                if (end == std::string_view::npos) {
                    return std::unexpected(
                        error_at(task.declaration.location, "unterminated command interpolation in `" + std::string(value) + "`")
                    );
                }

                const std::string_view name = value.substr(position + 2, end - position - 2);
                if (name == "workspace" || name == "package-dir" || name == "state-dir" || name == "profile" || name == "artifact") {
                    position = end + 1;
                    continue;
                }
                if (name == "build-dir" || name == "output-dir") {
                    require_default_build(analysis.build, task.package);
                    position = end + 1;
                    continue;
                }
                if (name.starts_with("artifact:") && name.size() != std::string_view("artifact:").size()) {
                    position = end + 1;
                    continue;
                }
                if (name.starts_with("target:") && name.size() != std::string_view("target:").size()) {
                    require_target_build(analysis.build, task.package, name.substr(std::string_view("target:").size()));
                    analysis.requires_products = true;
                    position = end + 1;
                    continue;
                }

                return std::unexpected(error_at(task.declaration.location, "unknown command interpolation `${" + std::string(name) + "}`"));
            }
            return {};
        }

        Result<void> analyze_task_values(TaskAnalysis& analysis, const TaskDefinition& task) {
            for (const std::string& argument: task.declaration.run) {
                auto analyzed = analyze_placeholders(analysis, task, argument);
                if (!analyzed)
                    return std::unexpected(analyzed.error());
            }
            if (task.declaration.working_directory) {
                auto analyzed = analyze_placeholders(analysis, task, task.declaration.working_directory->generic_string());
                if (!analyzed)
                    return std::unexpected(analyzed.error());
            }
            for (const auto& [name, value]: task.declaration.environment) {
                auto analyzed = analyze_placeholders(analysis, task, value);
                if (!analyzed)
                    return std::unexpected(analyzed.error());
            }
            for (const std::filesystem::path& path: task.declaration.inputs) {
                auto analyzed = analyze_placeholders(analysis, task, path.generic_string());
                if (!analyzed)
                    return std::unexpected(analyzed.error());
            }
            for (const std::filesystem::path& path: task.declaration.outputs) {
                auto analyzed = analyze_placeholders(analysis, task, path.generic_string());
                if (!analyzed)
                    return std::unexpected(analyzed.error());
            }
            return {};
        }

        Result<void> visit_task(TaskAnalysis& analysis, const Graph& graph, const std::size_t index) {
            if (analysis.states[index] == 2)
                return {};

            const TaskDefinition& task = analysis.available[index];
            if (analysis.states[index] == 1) {
                return std::unexpected(
                    error_at(task.declaration.location, "command dependency cycle reaches `" + task.qualified_name + "`")
                );
            }
            analysis.states[index] = 1;
            for (const std::string& dependency: task.declaration.after) {
                if (dependency == "build") {
                    require_default_build(analysis.build, task.package);
                    continue;
                }
                if (dependency.starts_with("build:")) {
                    const std::string_view target = std::string_view(dependency).substr(std::string_view("build:").size());
                    if (target.empty()) {
                        return std::unexpected(error_at(task.declaration.location, "command dependency `build:` requires a target name"));
                    }
                    require_target_build(analysis.build, task.package, target);
                    continue;
                }

                auto resolved = resolve_task(analysis.available, graph, dependency, task.package);
                if (!resolved)
                    return std::unexpected(std::move(resolved).error().add_note("required by command `" + task.qualified_name + "`"));

                auto visited = visit_task(analysis, graph, *resolved);
                if (!visited)
                    return std::unexpected(visited.error());
            }

            auto values = analyze_task_values(analysis, task);
            if (!values)
                return std::unexpected(values.error());

            analysis.states[index] = 2;
            analysis.ordered.push_back(index);
            return {};
        }

        Result<TaskAnalysis> analyze_task(
            const Graph& graph,
            const std::string_view requested,
            const std::optional<PackageId> relative_package
        ) {
            auto tasks = discover_tasks(graph);
            if (!tasks)
                return std::unexpected(tasks.error());

            auto selected = resolve_task(*tasks, graph, requested, relative_package);
            if (!selected)
                return std::unexpected(selected.error());

            TaskAnalysis analysis;
            analysis.available = std::move(*tasks);
            analysis.states.resize(analysis.available.size());
            analysis.build.build_default = false;
            auto visited = visit_task(analysis, graph, *selected);
            if (!visited)
                return std::unexpected(visited.error());

            return analysis;
        }

        Result<std::string> configured_artifact(
            const Graph& graph,
            const std::span<const ConfiguredPackageInstance> instances,
            const TaskDefinition& task,
            const std::string_view requested_package
        ) {
            const auto package = graph.find_by_name(requested_package);
            if (!package)
                return std::unexpected(error_at(
                    task.declaration.location,
                    "unknown package `" + std::string(requested_package) + "` in artifact interpolation"
                ));

            std::string context = graph[*package].name + ":default";
            if (*package == task.package && task.associated_target)
                context = *task.associated_target;

            const ConfiguredPackageInstance* instance = find_configured_package_instance(instances, *package, context);
            if (!instance)
                return std::unexpected(
                    error_at(task.declaration.location, "configured artifact is missing for package `" + graph[*package].name + "`")
                );

            return instance->artifact;
        }

        Result<std::string> product_artifact(
            const Graph& graph,
            const std::span<const BuildProduct> products,
            const TaskDefinition& task,
            const std::string_view name
        ) {
            const auto local = std::ranges::find_if(products, [&](const BuildProduct& product) {
                return product.package == task.package && product.name == name;
            });
            if (local != products.end()) {
                if (!local->artifact)
                    return std::unexpected(
                        error_at(task.declaration.location, "target `" + std::string(name) + "` has no executable artifact")
                    );

                return local->artifact->string();
            }

            const BuildProduct* match = nullptr;
            for (const BuildProduct& product: products) {
                if (product.name != name)
                    continue;

                if (match) {
                    return std::unexpected(
                        error_at(task.declaration.location, "target `" + std::string(name) + "` is provided by multiple selected packages")
                    );
                }
                match = &product;
            }
            if (!match)
                return std::unexpected(error_at(task.declaration.location, "target `" + std::string(name) + "` is unavailable"));

            if (!match->artifact)
                return std::unexpected(
                    error_at(task.declaration.location, "target `" + std::string(name) + "` has no executable artifact")
                );

            return match->artifact->string();
        }

        Result<std::string> build_directory(const std::span<const BuildOutput> outputs, const TaskDefinition& task) {
            const BuildOutput* match = nullptr;
            for (const BuildOutput& output: outputs) {
                if (output.package != task.package)
                    continue;

                if (match && match->path != output.path) {
                    return std::unexpected(error_at(task.declaration.location, "`${build-dir}` is ambiguous across configured variants"));
                }
                match = &output;
            }
            if (!match)
                return std::unexpected(error_at(task.declaration.location, "`${build-dir}` requires a build of the command package"));

            if (!match->build_directory)
                return std::unexpected(error_at(task.declaration.location, "resolver does not expose a backend build directory"));

            return match->build_directory->string();
        }

        Result<std::string> output_directory(const std::span<const BuildOutput> outputs, const TaskDefinition& task) {
            const BuildOutput* match = nullptr;
            for (const BuildOutput& output: outputs) {
                if (output.package != task.package)
                    continue;

                if (match && match->path != output.path) {
                    return std::unexpected(error_at(task.declaration.location, "`${output-dir}` is ambiguous across configured variants"));
                }
                match = &output;
            }
            if (!match)
                return std::unexpected(error_at(task.declaration.location, "`${output-dir}` requires a build of the command package"));

            return match->path.string();
        }

        Result<std::string> interpolate(
            const Graph& graph,
            const BuildEnvironment& environment,
            const std::span<const ConfiguredPackageInstance> instances,
            const std::span<const BuildOutput> outputs,
            const std::span<const BuildProduct> products,
            const TaskDefinition& task,
            std::string value
        ) {
            std::size_t position = 0;
            while ((position = value.find("${", position)) != std::string::npos) {
                const std::size_t end = value.find('}', position + 2);
                if (end == std::string::npos)
                    return std::unexpected(error_at(task.declaration.location, "unterminated command interpolation"));

                const std::string name = value.substr(position + 2, end - position - 2);
                Result<std::string> replacement = std::unexpected(
                    error_at(task.declaration.location, "unknown command interpolation `${" + name + "}`")
                );
                if (name == "workspace")
                    replacement = environment.workspace.string();
                else if (name == "package-dir")
                    replacement = graph[task.package].directory.string();
                else if (name == "state-dir")
                    replacement = environment.state_root.string();
                else if (name == "profile")
                    replacement = environment.configuration.profile;
                else if (name == "build-dir")
                    replacement = build_directory(outputs, task);
                else if (name == "output-dir")
                    replacement = output_directory(outputs, task);
                else if (name == "artifact")
                    replacement = configured_artifact(graph, instances, task, graph[task.package].name);
                else if (name.starts_with("artifact:"))
                    replacement = configured_artifact(
                        graph,
                        instances,
                        task,
                        std::string_view(name).substr(std::string_view("artifact:").size())
                    );
                else if (name.starts_with("target:"))
                    replacement = product_artifact(
                        graph,
                        products,
                        task,
                        std::string_view(name).substr(std::string_view("target:").size())
                    );

                if (!replacement)
                    return std::unexpected(replacement.error());

                value.replace(position, end - position + 1, *replacement);
                position += replacement->size();
            }
            return value;
        }

        Result<std::vector<std::filesystem::path>> command_inputs(
            const TaskDeclaration& declaration,
            const std::filesystem::path& working_directory,
            const std::vector<std::string>& patterns
        ) {
            FileSet files;
            files.include = patterns;
            files.location = declaration.location;
            auto expanded = expand_file_set(files, working_directory, working_directory, false);
            if (!expanded)
                return std::unexpected(expanded.error());

            for (std::filesystem::path& path: *expanded) {
                if (path.is_relative())
                    path = working_directory / path;
            }
            return expanded;
        }

        struct CommandPlanningContext {
            const Graph& graph;
            const BuildEnvironment& environment;
            std::span<const ConfiguredPackageInstance> instances;
            std::span<const BuildProduct> products;
            std::span<const std::string> extra_arguments;
            std::string_view selected;
        };

        Result<void> append_command_action(BuildPlan& plan, const CommandPlanningContext& context, const TaskDefinition& task) {
            std::filesystem::path working_directory = task.declaration.source.parent_path();
            if (working_directory.empty())
                working_directory = context.graph[task.package].directory;

            if (task.declaration.working_directory) {
                auto interpolated = interpolate(
                    context.graph,
                    context.environment,
                    context.instances,
                    plan.outputs(),
                    context.products,
                    task,
                    task.declaration.working_directory->generic_string()
                );
                if (!interpolated)
                    return std::unexpected(interpolated.error());

                std::filesystem::path declared = *interpolated;
                working_directory = declared.is_absolute() ? declared : working_directory / declared;
            }
            working_directory = working_directory.lexically_normal();

            Action action;
            action.description = "task " + task.qualified_name;
            for (const std::string& declared: task.declaration.run) {
                auto argument = interpolate(
                    context.graph,
                    context.environment,
                    context.instances,
                    plan.outputs(),
                    context.products,
                    task,
                    declared
                );
                if (!argument)
                    return std::unexpected(argument.error());

                if (!is_glob_pattern(*argument)) {
                    action.argv.push_back(std::move(*argument));
                    continue;
                }

                FileSet files;
                files.include.push_back(*argument);
                files.location = task.declaration.location;
                auto expanded = expand_file_set(files, working_directory, working_directory);
                if (!expanded)
                    return std::unexpected(expanded.error());

                for (const std::filesystem::path& path: *expanded)
                    action.argv.push_back(path.string());
            }
            if (task.qualified_name == context.selected)
                action.argv.insert(action.argv.end(), context.extra_arguments.begin(), context.extra_arguments.end());

            action.working_directory = working_directory;
            for (const auto& [name, declared]: task.declaration.environment) {
                auto value = interpolate(
                    context.graph,
                    context.environment,
                    context.instances,
                    plan.outputs(),
                    context.products,
                    task,
                    declared
                );
                if (!value)
                    return std::unexpected(value.error());

                action.environment.push_back({name, std::move(*value)});
            }

            std::vector<std::string> inputs;
            inputs.reserve(task.declaration.inputs.size());
            for (const std::filesystem::path& declared: task.declaration.inputs) {
                auto input = interpolate(
                    context.graph,
                    context.environment,
                    context.instances,
                    plan.outputs(),
                    context.products,
                    task,
                    declared.generic_string()
                );
                if (!input)
                    return std::unexpected(input.error());

                inputs.push_back(std::move(*input));
            }
            auto expanded_inputs = command_inputs(task.declaration, working_directory, inputs);
            if (!expanded_inputs)
                return std::unexpected(expanded_inputs.error());

            action.inputs = std::move(*expanded_inputs);
            for (const std::filesystem::path& declared: task.declaration.outputs) {
                auto output = interpolate(
                    context.graph,
                    context.environment,
                    context.instances,
                    plan.outputs(),
                    context.products,
                    task,
                    declared.generic_string()
                );
                if (!output)
                    return std::unexpected(output.error());

                if (is_glob_pattern(*output)) {
                    return std::unexpected(
                        error_at(task.declaration.location, "command outputs cannot be glob patterns: `" + *output + "`")
                    );
                }
                std::filesystem::path path = *output;
                action.outputs.push_back((path.is_absolute() ? path : working_directory / path).lexically_normal());
            }

            action.package = task.package;
            std::string configured_context = context.graph[task.package].name + ":default";
            if (task.associated_target)
                configured_context = *task.associated_target;

            if (const ConfiguredPackageInstance*
                    instance = find_configured_package_instance(context.instances, task.package, configured_context)) {
                action.configured_artifact = instance->artifact;
            }

            action.stage = ActionStage::task;
            plan.add(std::move(action));
            return {};
        }
    }

    Result<std::vector<TaskDefinition>> discover_tasks(const Graph& graph) {
        std::vector<TaskDefinition> result;
        for (const PackageId root: graph.roots()) {
            const PackageNode& package = graph[root];
            if (!package.manifest)
                continue;

            for (const TaskDeclaration& declaration: package.manifest->commands) {
                auto appended = append_task(result, graph, package, declaration, std::nullopt);
                if (!appended)
                    return std::unexpected(appended.error());
            }
            for (const PackageTarget& target: package.targets) {
                for (const TaskDeclaration& declaration: target.commands) {
                    auto appended = append_task(result, graph, package, declaration, target.name);
                    if (!appended)
                        return std::unexpected(appended.error());
                }
            }
        }
        return result;
    }

    Result<TaskPreparation> prepare_task(
        const Graph& graph,
        const std::string_view requested,
        const std::optional<PackageId> relative_package
    ) {
        auto analysis = analyze_task(graph, requested, relative_package);
        if (!analysis)
            return std::unexpected(analysis.error());

        TaskPreparation result;
        result.build = std::move(analysis->build);
        result.requires_products = analysis->requires_products;
        result.tasks.reserve(analysis->ordered.size());
        for (const std::size_t index: analysis->ordered)
            result.tasks.push_back(std::move(analysis->available[index]));

        result.selected = result.tasks.back().qualified_name;
        return result;
    }

    Result<BuildPlan> plan_task(
        const Graph& graph,
        const ExtensionRegistry& registry,
        const BuildEnvironment& environment,
        const TaskPreparation& preparation,
        const std::span<const BuildProduct> products,
        const std::span<const std::string> arguments
    ) {
        if (preparation.tasks.empty())
            return std::unexpected(error("task preparation contains no commands"));

        BuildPlan plan;
        if (!preparation.build.packages.empty()) {
            auto build = plan_build(graph, registry, environment, preparation.build);
            if (!build)
                return std::unexpected(build.error());

            plan.append(std::move(*build));
        }

        auto instances = configure_package_instances(graph, {environment.configuration.profile, host_target_os()});
        if (!instances)
            return std::unexpected(instances.error());

        const CommandPlanningContext context{graph, environment, *instances, products, arguments, preparation.selected};
        for (const TaskDefinition& task: preparation.tasks) {
            auto appended = append_command_action(plan, context, task);
            if (!appended)
                return std::unexpected(appended.error());
        }
        return plan;
    }
}
