#include "application_internal.hpp"

#include <kaixa/model/effective_product.hpp>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace kaixa::cli::detail {
    Result<void> validate_build_targets(const std::span<const BuildProduct> products, const std::span<const std::string> requested) {
        for (const std::string& name: requested) {
            const std::size_t matches = static_cast<std::size_t>(std::ranges::count(products, name, &BuildProduct::name));
            if (matches == 0) {
                std::string available;
                for (const BuildProduct& product: products) {
                    if (!available.empty())
                        available += ", ";

                    available += product.name;
                }

                Diagnostic diagnostic = error("build target `" + name + "` does not exist");
                if (!available.empty())
                    return std::unexpected(std::move(diagnostic).add_note("available targets: " + available));

                return std::unexpected(std::move(diagnostic));
            }
            if (matches > 1) {
                return std::unexpected(error("build target `" + name + "` is provided by multiple selected packages")
                        .add_note("narrow the operation with `--package <name>`"));
            }
        }
        return {};
    }

    struct ResolvedProductSelection {
        std::vector<PackageBuildRequest> packages;
        std::vector<std::string> displayed;
    };

    PackageBuildRequest& package_request(std::vector<PackageBuildRequest>& requests, const PackageId package) {
        const auto existing = std::ranges::find(requests, package, &PackageBuildRequest::package);
        if (existing != requests.end())
            return *existing;

        requests.push_back({package, {}, false});
        return requests.back();
    }

    void append_product_target(const BuildProduct& product, std::vector<PackageBuildRequest>& requests) {
        PackageBuildRequest& request = package_request(requests, product.package);
        if (std::ranges::find(request.targets, product.name) == request.targets.end())
            request.targets.push_back(product.name);
    }

    void append_products(
        const std::span<const BuildProduct> products,
        const ProductPurpose purpose,
        std::vector<PackageBuildRequest>& output
    ) {
        for (const BuildProduct& product: products) {
            if (product.purpose == purpose)
                append_product_target(product, output);
        }
    }

    Result<void> append_named_products(
        const std::span<const BuildProduct> products,
        const std::span<const std::string> requested,
        const ProductPurpose purpose,
        const std::string_view option,
        std::vector<PackageBuildRequest>& output
    ) {
        for (const std::string& name: requested) {
            const auto named = std::ranges::find(products, name, &BuildProduct::name);
            if (named == products.end()) {
                Diagnostic diagnostic = error(
                    std::string(option) + " selected unknown " + std::string(product_purpose_name(purpose)) + " `" + name + "`"
                );
                std::string available;
                for (const BuildProduct& candidate: products) {
                    if (candidate.purpose != purpose)
                        continue;

                    if (!available.empty())
                        available += ", ";

                    available += candidate.name;
                }
                if (!available.empty()) {
                    return std::unexpected(
                        std::move(diagnostic)
                            .add_note("available " + std::string(product_purpose_name(purpose)) + " products: " + available)
                    );
                }

                return std::unexpected(std::move(diagnostic));
            }
            const auto product = std::ranges::find_if(products, [&](const BuildProduct& candidate) {
                return candidate.name == name && candidate.purpose == purpose;
            });
            if (product == products.end()) {
                return std::unexpected(error(
                    "`"
                    + name
                    + "` has purpose `"
                    + std::string(product_purpose_name(named->purpose))
                    + "`, not `"
                    + std::string(product_purpose_name(purpose))
                    + "`"
                ));
            }

            const std::size_t matches = static_cast<std::size_t>(std::ranges::count_if(products, [&](const BuildProduct& candidate) {
                return candidate.name == name && candidate.purpose == purpose;
            }));
            if (matches > 1) {
                return std::unexpected(
                    error(std::string(option) + " selected `" + name + "`, which is provided by multiple selected packages")
                        .add_note("narrow the operation with `--package <name>`")
                );
            }

            append_product_target(*product, output);
        }
        return {};
    }

    Result<ResolvedProductSelection> resolve_product_selection(const std::span<const BuildProduct> products, const BuildCommand& command) {
        ResolvedProductSelection result;
        if (!command.targets.empty()) {
            auto valid = validate_build_targets(products, command.targets);
            if (!valid)
                return std::unexpected(valid.error());

            result.displayed = command.targets;
            for (const std::string& name: command.targets) {
                const auto product = std::ranges::find(products, name, &BuildProduct::name);
                append_product_target(*product, result.packages);
            }
            return result;
        }

        if (command.selection.empty()) {
            for (const BuildProduct& product: products) {
                if (product.purpose == ProductPurpose::primary)
                    result.displayed.push_back(product.name);
            }
            return result;
        }

        if (command.selection.all_targets) {
            for (const BuildProduct& product: products) {
                result.displayed.push_back(product.name);
                PackageBuildRequest& request = package_request(result.packages, product.package);
                if (product.purpose == ProductPurpose::primary)
                    request.build_default = true;
                else
                    append_product_target(product, result.packages);
            }
            return result;
        }

        if (command.selection.all_examples)
            append_products(products, ProductPurpose::example, result.packages);

        if (command.selection.all_tests)
            append_products(products, ProductPurpose::test, result.packages);

        if (command.selection.all_benchmarks)
            append_products(products, ProductPurpose::benchmark, result.packages);

        auto examples = append_named_products(products, command.selection.examples, ProductPurpose::example, "--example", result.packages);
        if (!examples)
            return std::unexpected(examples.error());

        auto tests = append_named_products(products, command.selection.tests, ProductPurpose::test, "--test", result.packages);
        if (!tests)
            return std::unexpected(tests.error());

        auto benchmarks = append_named_products(
            products,
            command.selection.benchmarks,
            ProductPurpose::benchmark,
            "--bench",
            result.packages
        );
        if (!benchmarks)
            return std::unexpected(benchmarks.error());

        if (result.packages.empty() && !command.list) {
            return std::unexpected(error("the selected product categories contain no targets"));
        }

        for (const PackageBuildRequest& package: result.packages) {
            result.displayed.insert(result.displayed.end(), package.targets.begin(), package.targets.end());
        }
        return result;
    }

    Result<std::vector<RunTarget>> select_runnable_category(
        const std::span<const RunTarget> targets,
        const ProductPurpose purpose,
        const std::optional<std::string>& requested
    ) {
        if (requested) {
            const auto product = std::ranges::find(targets, *requested, &RunTarget::name);
            if (product != targets.end() && product->purpose != purpose) {
                std::string expected;
                if (product->purpose == ProductPurpose::example)
                    expected = "kaixa run --example " + *requested;
                else if (product->purpose == ProductPurpose::benchmark)
                    expected = "kaixa bench --target " + *requested;
                else
                    expected = "kaixa run --target " + *requested;

                return std::unexpected(
                    error("`" + *requested + "` is a " + std::string(product_purpose_name(product->purpose)) + " product")
                        .add_note("select it with `" + expected + "`")
                );
            }
        }

        std::vector<RunTarget> result;
        for (const RunTarget& target: targets) {
            if (target.purpose == purpose)
                result.push_back(target);
        }
        return result;
    }

    int execute_and_run_target(
        const ExecutionPlan& plan,
        RunTarget selected,
        const std::span<const std::string> arguments,
        const std::string_view operation
    ) {
        auto printed = print_actions(plan);
        if (!printed)
            return fail(printed.error());

        auto built = kaixa::execute(plan);
        if (!built)
            return fail(built.error());

        selected.process.argv.insert(selected.process.argv.end(), arguments.begin(), arguments.end());
        std::cout << operation << ": " << format_command(selected.process.argv) << '\n';
        std::cout.flush();

        auto result = run_process(selected.process);
        if (!result)
            return fail(result.error());

        return result->exit_code;
    }

    int build_and_run_target(
        const Workspace& workspace,
        RunTarget selected,
        const std::span<const std::string> arguments,
        const std::string_view operation
    ) {
        auto plan = plan_run(
            workspace.graph,
            workspace.registry,
            workspace.environment,
            selected.name,
            selected.package,
            workspace.instances
        );
        if (!plan)
            return fail(plan.error());

        return execute_and_run_target(*plan, std::move(selected), arguments, operation);
    }

    Result<std::size_t> execute_task_preparation(
        const Workspace& workspace,
        const TaskPreparation& preparation,
        const std::span<const std::string> arguments = {}
    ) {
        std::vector<BuildProduct> products;
        if (preparation.requires_products) {
            auto synchronization = plan_build(
                workspace.graph,
                workspace.registry,
                workspace.environment,
                preparation.build,
                workspace.instances
            );
            if (!synchronization)
                return std::unexpected(synchronization.error());

            auto printed = print_actions(*synchronization, true);
            if (!printed)
                return std::unexpected(printed.error());

            auto generated = generate(*synchronization);
            if (!generated)
                return std::unexpected(generated.error());

            auto discovered = discover_products(workspace.graph, workspace.registry, workspace.environment, workspace.instances);
            if (!discovered)
                return std::unexpected(discovered.error());

            products = std::move(*discovered);
        }

        auto plan = plan_task(
            workspace.graph,
            workspace.registry,
            workspace.environment,
            preparation,
            products,
            arguments,
            workspace.instances
        );
        if (!plan)
            return std::unexpected(plan.error());

        auto printed = print_actions(*plan);
        if (!printed)
            return std::unexpected(printed.error());

        auto report = kaixa::execute(*plan);
        if (!report)
            return std::unexpected(report.error());

        return report->executed;
    }

    Result<std::size_t> execute_workflow_generate(const Workspace& workspace) {
        auto plan = plan_build(workspace.graph, workspace.registry, workspace.environment, {}, workspace.instances);
        if (!plan)
            return std::unexpected(plan.error());

        auto printed = print_actions(*plan, true);
        if (!printed)
            return std::unexpected(printed.error());

        auto report = generate(*plan);
        if (!report)
            return std::unexpected(report.error());

        return report->synchronized;
    }

    Result<std::size_t> execute_workflow_build(const Workspace& workspace) {
        BuildRequest request;
        request.build_default = true;
        auto plan = plan_build(workspace.graph, workspace.registry, workspace.environment, request, workspace.instances);
        if (!plan)
            return std::unexpected(plan.error());

        auto printed = print_actions(*plan);
        if (!printed)
            return std::unexpected(printed.error());

        auto report = kaixa::execute(*plan);
        if (!report)
            return std::unexpected(report.error());

        return report->executed;
    }

    Result<std::size_t> execute_workflow_tests(const Workspace& workspace) {
        auto plan = plan_tests(workspace.graph, workspace.registry, workspace.environment, TestRequest{}, workspace.instances);
        if (!plan)
            return std::unexpected(plan.error());

        auto printed = print_actions(*plan);
        if (!printed)
            return std::unexpected(printed.error());

        auto report = test(*plan);
        if (!report)
            return std::unexpected(report.error());

        return report->executed;
    }

    Result<std::size_t> execute_workflow_benchmarks(const Workspace& workspace) {
        TestRequest request;
        request.purpose = ProductPurpose::benchmark;
        auto plan = plan_tests(workspace.graph, workspace.registry, workspace.environment, request, workspace.instances);
        if (!plan)
            return std::unexpected(plan.error());

        auto printed = print_actions(*plan);
        if (!printed)
            return std::unexpected(printed.error());

        auto report = test(*plan);
        if (!report)
            return std::unexpected(report.error());

        return report->executed;
    }

    Result<std::size_t> execute_workflow_step(const Workspace& workspace, const PreparedWorkflowStep& step) {
        switch (step.kind) {
        case WorkflowStepKind::generate: return execute_workflow_generate(workspace);
        case WorkflowStepKind::build: return execute_workflow_build(workspace);
        case WorkflowStepKind::test: return execute_workflow_tests(workspace);
        case WorkflowStepKind::bench: return execute_workflow_benchmarks(workspace);
        case WorkflowStepKind::task:
            if (!step.task)
                return std::unexpected(error("prepared workflow task has no task plan"));

            return execute_task_preparation(workspace, *step.task);
        }
        return std::unexpected(error("unknown workflow step kind"));
    }

    int run(const BuildCommand& command) {
        auto workspace = open_workspace(command.workspace);
        if (!workspace)
            return fail(workspace.error());

        ResolvedProductSelection selection;
        bool selection_resolved = false;
        if (command.list || !command.targets.empty() || !command.selection.empty()) {
            auto synchronization = plan_build(workspace->graph, workspace->registry, workspace->environment, {}, workspace->instances);
            if (!synchronization)
                return fail(synchronization.error());

            auto printed = print_actions(*synchronization, true);
            if (!printed)
                return fail(printed.error());

            auto generated = generate(*synchronization);
            if (!generated)
                return fail(generated.error());

            auto products = discover_products(workspace->graph, workspace->registry, workspace->environment, workspace->instances);
            if (!products)
                return fail(products.error());

            auto resolved = resolve_product_selection(*products, command);
            if (!resolved)
                return fail(resolved.error());

            selection = std::move(*resolved);
            selection_resolved = true;
            if (command.list) {
                if (selection.displayed.empty()) {
                    std::cout << "no matching products\n";
                    return 0;
                }

                print_products(*products, workspace->environment.workspace, selection.displayed, true);
                return 0;
            }
        }

        BuildRequest request;
        request.jobs = command.jobs;
        request.build_default = true;
        if (selection_resolved)
            request.packages = selection.packages;

        auto plan = plan_build(workspace->graph, workspace->registry, workspace->environment, request, workspace->instances);
        if (!plan)
            return fail(plan.error());

        auto printed = print_actions(*plan);
        if (!printed)
            return fail(printed.error());

        auto report = kaixa::execute(*plan);
        if (!report)
            return fail(report.error());

        std::cout << "build completed: " << report->executed << " action(s) run\n";
        auto products = discover_products(workspace->graph, workspace->registry, workspace->environment, workspace->instances);
        if (!products)
            return fail(products.error());

        if (!selection_resolved) {
            auto resolved = resolve_product_selection(*products, command);
            if (!resolved)
                return fail(resolved.error());

            selection = std::move(*resolved);
        }
        print_products(*products, workspace->environment.workspace, selection.displayed, true);
        return 0;
    }

    int run(const InstallCommand& command) {
        auto workspace = open_workspace(command.workspace);
        if (!workspace)
            return fail(workspace.error());

        std::filesystem::path prefix = command.prefix.value_or(
            workspace->environment.state_root / "install" / workspace->environment.configuration.profile
        );
        if (prefix.is_relative())
            prefix = workspace->environment.workspace / prefix;

        prefix = prefix.lexically_normal();
        BuildRequest request;
        request.install = true;
        request.install_prefix = prefix;
        auto plan = plan_build(workspace->graph, workspace->registry, workspace->environment, request, workspace->instances);
        if (!plan)
            return fail(plan.error());

        auto printed = print_actions(*plan);
        if (!printed)
            return fail(printed.error());

        auto report = execute(*plan);
        if (!report)
            return fail(report.error());

        std::cout << "install completed: " << report->executed << " action(s) run\n";
        std::cout << "prefix -> " << display_path(prefix, workspace->environment.workspace) << '\n';
        return 0;
    }

    int run(const TestCommand& command) {
        auto workspace = open_workspace(command.workspace);
        if (!workspace)
            return fail(workspace.error());

        auto plan = plan_tests(workspace->graph, workspace->registry, workspace->environment, command.request, workspace->instances);
        if (!plan)
            return fail(plan.error());

        auto printed = print_actions(*plan);
        if (!printed)
            return fail(printed.error());

        auto report = test(*plan);
        if (!report)
            return fail(report.error());

        std::cout << "tests completed: " << report->executed << " action(s) run\n";
        print_outputs(*plan, workspace->environment.workspace);
        return 0;
    }

    int run(const BenchCommand& command) {
        auto workspace = open_workspace(command.workspace);
        if (!workspace)
            return fail(workspace.error());

        if (command.arguments.empty()) {
            TestRequest request;
            request.target = command.target;
            request.mode = command.list ? TestMode::list : TestMode::run;
            request.purpose = ProductPurpose::benchmark;
            auto plan = plan_tests(workspace->graph, workspace->registry, workspace->environment, request, workspace->instances);
            if (!plan)
                return fail(plan.error());

            auto printed = print_actions(*plan);
            if (!printed)
                return fail(printed.error());

            auto report = test(*plan);
            if (!report)
                return fail(report.error());

            if (!command.list)
                std::cout << "benchmarks completed: " << report->executed << " action(s) run\n";

            return 0;
        }

        auto synchronization = plan_build(workspace->graph, workspace->registry, workspace->environment, {}, workspace->instances);
        if (!synchronization)
            return fail(synchronization.error());

        auto printed = print_actions(*synchronization, true);
        if (!printed)
            return fail(printed.error());

        auto generated = generate(*synchronization);
        if (!generated)
            return fail(generated.error());

        auto targets = discover_executable_targets(workspace->graph, workspace->registry, workspace->environment, workspace->instances);
        if (!targets)
            return fail(targets.error());

        auto benchmarks = select_runnable_category(*targets, ProductPurpose::benchmark, command.target);
        if (!benchmarks)
            return fail(benchmarks.error());

        if (command.list) {
            if (benchmarks->empty()) {
                std::cout << "no runnable benchmarks\n";
                return 0;
            }

            for (const RunTarget& target: *benchmarks)
                std::cout << target.name << '\n';

            return 0;
        }

        std::string package_name;
        if (workspace->graph.roots().size() == 1) {
            package_name = workspace->graph[workspace->graph.roots().front()].name;
        } else if (!command.target) {
            auto root = require_single_root(workspace->graph, "bench");
            if (!root)
                return fail(root.error());

            package_name = workspace->graph[*root].name;
        }

        auto selected = select_run_target(*benchmarks, command.target, package_name);
        if (!selected)
            return fail(selected.error());

        return build_and_run_target(*workspace, std::move(*selected), command.arguments, "benchmarking");
    }

    int run(const RunCommand& command) {
        auto workspace = open_workspace(command.workspace);
        if (!workspace)
            return fail(workspace.error());

        auto synchronization = plan_build(workspace->graph, workspace->registry, workspace->environment, {}, workspace->instances);
        if (!synchronization)
            return fail(synchronization.error());

        auto printed = print_actions(*synchronization, true);
        if (!printed)
            return fail(printed.error());

        auto generated = generate(*synchronization);
        if (!generated)
            return fail(generated.error());

        auto targets = discover_run_targets(workspace->graph, workspace->registry, workspace->environment, workspace->instances);
        if (!targets)
            return fail(targets.error());

        const ProductPurpose purpose = command.examples || command.example ? ProductPurpose::example : ProductPurpose::primary;
        const std::optional<std::string>& requested = command.example ? command.example : command.target;
        auto category = select_runnable_category(*targets, purpose, requested);
        if (!category)
            return fail(category.error());

        if (command.list) {
            if (category->empty()) {
                std::cout << (purpose == ProductPurpose::example ? "no runnable examples\n" : "no runnable targets\n");
                return 0;
            }

            for (const RunTarget& target: *category)
                std::cout << target.name << '\n';

            return 0;
        }

        std::string package_name;
        if (workspace->graph.roots().size() == 1) {
            package_name = workspace->graph[workspace->graph.roots().front()].name;
        } else if (!requested) {
            auto root = require_single_root(workspace->graph, "run");
            if (!root)
                return fail(root.error());

            package_name = workspace->graph[*root].name;
        }

        auto selected = select_run_target(*category, requested, package_name);
        if (!selected)
            return fail(selected.error());

        if (!requested && purpose == ProductPurpose::primary) {
            return execute_and_run_target(*synchronization, std::move(*selected), command.arguments, "running");
        }

        return build_and_run_target(*workspace, std::move(*selected), command.arguments, "running");
    }

    int run(const TaskCommand& command) {
        auto workspace = open_workspace(command.workspace);
        if (!workspace)
            return fail(workspace.error());

        auto tasks = discover_tasks(workspace->graph);
        if (!tasks)
            return fail(tasks.error());

        if (command.list) {
            if (tasks->empty()) {
                std::cout << "no custom tasks\n";
                return 0;
            }
            for (const TaskDefinition& task: *tasks)
                std::cout << task.qualified_name << '\n';

            return 0;
        }

        auto preparation = prepare_task(workspace->graph, *command.name);
        if (!preparation)
            return fail(preparation.error());

        auto report = execute_task_preparation(*workspace, *preparation, command.arguments);
        if (!report)
            return fail(report.error());

        std::cout << "task completed: " << *report << " action(s) run\n";
        return 0;
    }

    int run(const WorkflowCommand& command) {
        auto workspace = open_workspace(command.workspace);
        if (!workspace)
            return fail(workspace.error());

        auto workflows = discover_workflows(workspace->graph);
        if (!workflows)
            return fail(workflows.error());

        if (command.list) {
            if (workflows->empty()) {
                std::cout << "no workflows\n";
                return 0;
            }
            for (const WorkflowDefinition& workflow: *workflows)
                std::cout << workflow.qualified_name << '\n';

            return 0;
        }

        auto preparation = prepare_workflow(workspace->graph, *command.name);
        if (!preparation)
            return fail(preparation.error());

        std::size_t executed = 0;
        for (std::size_t index = 0; index < preparation->steps.size(); ++index) {
            const PreparedWorkflowStep& step = preparation->steps[index];
            std::cout
                << "workflow "
                << preparation->workflow.qualified_name
                << ": step "
                << index + 1
                << '/'
                << preparation->steps.size()
                << ' '
                << step.name
                << '\n';
            std::cout.flush();

            auto result = execute_workflow_step(*workspace, step);
            if (!result) {
                return fail(
                    std::move(result).error().add_note(
                        "workflow `" + preparation->workflow.qualified_name + "` failed at step `" + step.name + "`"
                    )
                );
            }
            executed += *result;
        }

        std::cout << "workflow completed: " << preparation->steps.size() << " step(s), " << executed << " action(s) run\n";
        return 0;
    }

}
