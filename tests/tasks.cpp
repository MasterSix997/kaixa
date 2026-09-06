#include <kaixa/foundation/filesystem.hpp>
#include <kaixa/kaixa.hpp>
#include <kaixa/plugin/bundle.hpp>
#include <test_support.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

KAIXA_TEST(manifest_normalizes_custom_command_forms) {
    const auto manifest = kaixa::parse_manifest_document_string(
        "[package]\n"
        "name = \"tasks\"\n"
        "\n"
        "[command.direct]\n"
        "run = [\"cmake\", \"-E\", \"echo\", \"ok\"]\n"
        "working-directory = \"tools\"\n"
        "inputs = [\"input.txt\"]\n"
        "outputs = [\"output.txt\"]\n"
        "after = [\"prepare\"]\n"
        "environment = { MODE = \"strict\" }\n"
        "\n"
        "[command.prepare]\n"
        "tool = \"powershell\"\n"
        "script = \"Prepare.ps1\"\n",
        "commands.toml"
    );
    context.check(manifest.has_value(), "custom command forms parse");
    if (!manifest) {
        context.fail(kaixa::format_diagnostic(manifest.error()));
        return;
    }

    context.check_equal(manifest->package->commands.size(), std::size_t{2}, "both commands are normalized");
    const kaixa::TaskDeclaration& direct = manifest->package->commands.front();
    context.check_equal(direct.run.front(), std::string("cmake"), "direct argv keeps its executable");
    context.check_equal(direct.environment.at("MODE"), std::string("strict"), "environment table is retained");
    context.check_equal(direct.working_directory->generic_string(), std::string("tools"), "working directory is retained");
    context.check_equal(manifest->package->commands.back().run.size(), std::size_t{2}, "tool and script become argv");
    context.check_equal(manifest->package->commands.back().run.back(), std::string("Prepare.ps1"), "script is the first tool argument");
}

KAIXA_TEST(manifest_rejects_ambiguous_custom_command_invocation) {
    const auto manifest = kaixa::parse_manifest_document_string(
        "[package]\n"
        "name = \"tasks\"\n"
        "\n"
        "[command.invalid]\n"
        "run = [\"cmake\", \"-E\", \"echo\"]\n"
        "tool = \"powershell\"\n"
        "script = \"Invalid.ps1\"\n",
        "commands.toml"
    );
    context.check(!manifest.has_value(), "ambiguous invocation fails during manifest parsing");
    if (!manifest)
        context.check_contains(kaixa::format_diagnostic(manifest.error()), "cannot combine", "diagnostic explains the exclusive forms");
}

KAIXA_TEST(manifest_rejects_legacy_command_arrays) {
    const auto manifest = kaixa::parse_manifest_document_string(
        "[package]\n"
        "name = \"tasks\"\n"
        "\n"
        "[[command]]\n"
        "name = \"legacy\"\n"
        "run = [\"cmake\", \"-E\", \"echo\"]\n",
        "commands.toml"
    );
    context.check(!manifest.has_value(), "legacy command arrays are rejected");
    if (!manifest)
        context.check_contains(kaixa::format_diagnostic(manifest.error()), "expected a table", "diagnostic identifies the new shape");
}

KAIXA_TEST(manifest_parses_named_workflows) {
    const auto manifest = kaixa::parse_manifest_document_string(
        "[package]\n"
        "name = \"automation\"\n"
        "\n"
        "[workflow.dev]\n"
        "steps = [\"generate\", \"quality\", \"build\", \"test\"]\n"
        "\n"
        "[workflow.ci]\n"
        "steps = [\"quality\", \"build\", \"test\", \"task:tidy\"]\n",
        "workflows.toml"
    );
    context.check(manifest.has_value(), "workflow tables parse");
    if (!manifest) {
        context.fail(kaixa::format_diagnostic(manifest.error()));
        return;
    }

    context.check_equal(manifest->package->workflows.size(), std::size_t{2}, "both workflows are retained");
    context.check_equal(manifest->package->workflows.front().name, std::string("dev"), "workflow name comes from its table key");
    context.check_equal(manifest->package->workflows.front().steps.size(), std::size_t{4}, "ordered workflow steps are retained");
}

KAIXA_TEST(manifest_writer_preserves_commands_and_workflows) {
    const auto manifest = kaixa::parse_manifest_document_string(
        "[package]\n"
        "name = \"automation\"\n"
        "resolver = \"cmake\"\n"
        "\n"
        "[command.quality]\n"
        "run = [\"python\", \"tools/quality.py\"]\n"
        "environment = { MODE = \"strict\" }\n"
        "\n"
        "[workflow.dev]\n"
        "steps = [\"quality\", \"build\", \"test\"]\n",
        "workflows.toml"
    );
    context.check(manifest.has_value(), "automation manifest parses before formatting");
    if (!manifest)
        return;

    const auto formatted = kaixa::format_manifest(*manifest->package);
    context.check(formatted.has_value(), "automation manifest formats");
    if (!formatted) {
        context.fail(kaixa::format_diagnostic(formatted.error()));
        return;
    }

    const auto reparsed = kaixa::parse_manifest_document_string(*formatted, "formatted-workflows.toml");
    context.check(reparsed.has_value(), "formatted automation manifest parses again");
    if (!reparsed) {
        context.fail(kaixa::format_diagnostic(reparsed.error()));
        return;
    }

    context.check_equal(reparsed->package->commands.size(), std::size_t{1}, "formatted manifest retains its command");
    context.check_equal(reparsed->package->workflows.size(), std::size_t{1}, "formatted manifest retains its workflow");
    context.check_equal(reparsed->package->workflows.front().steps.back(), std::string("test"), "workflow step order survives formatting");
}

KAIXA_TEST(manifest_rejects_empty_workflows) {
    const auto manifest = kaixa::parse_manifest_document_string(
        "[package]\n"
        "name = \"automation\"\n"
        "\n"
        "[workflow.empty]\n"
        "steps = []\n",
        "workflows.toml"
    );
    context.check(!manifest.has_value(), "empty workflow fails during manifest parsing");
    if (!manifest)
        context.check_contains(
            kaixa::format_diagnostic(manifest.error()),
            "requires at least one step",
            "diagnostic explains the requirement"
        );
}

KAIXA_TEST(task_planning_orders_dependencies_and_executes_incrementally) {
    const kaixa::testing::TempDirectory workspace("custom-tasks");
    workspace.write(
        "Kaixa.toml",
        "[package]\n"
        "name = \"tasks\"\n"
        "\n"
        "[command.prepare]\n"
        "run = [\"cmake\", \"-E\", \"copy\", \"input.txt\", \"prepared.txt\"]\n"
        "working-directory = \"work\"\n"
        "inputs = [\"input.txt\"]\n"
        "outputs = [\"prepared.txt\"]\n"
        "environment = { KAIXA_TASK_PROFILE = \"${profile}\" }\n"
        "\n"
        "[command.finish]\n"
        "run = [\"cmake\", \"-E\", \"copy\", \"prepared.txt\", \"result.txt\"]\n"
        "working-directory = \"work\"\n"
        "inputs = [\"prepared.txt\"]\n"
        "outputs = [\"result.txt\"]\n"
        "after = [\"prepare\"]\n"
        "\n"
        "[command.environment]\n"
        "run = [\"cmake\", \"-P\", \"environment.cmake\"]\n"
        "working-directory = \"work\"\n"
        "inputs = [\"environment.cmake\"]\n"
        "outputs = [\"environment.txt\"]\n"
        "environment = { KAIXA_TASK_PROFILE = \"${profile}\" }\n"
    );
    workspace.write("work/input.txt", "custom task input\n");
    workspace.write("work/environment.cmake", "file(WRITE environment.txt \"$ENV{KAIXA_TASK_PROFILE}\")\n");

    const auto graph = kaixa::load_workspace(workspace.path());
    context.check(graph.has_value(), "task workspace loads");
    if (!graph) {
        context.fail(kaixa::format_diagnostic(graph.error()));
        return;
    }

    auto preparation = kaixa::prepare_task(*graph, "finish");
    context.check(preparation.has_value(), "task dependency closure prepares");
    if (!preparation) {
        context.fail(kaixa::format_diagnostic(preparation.error()));
        return;
    }
    context.check_equal(preparation->tasks.size(), std::size_t{2}, "dependency task is included");
    context.check_equal(preparation->tasks.front().declaration.name, std::string("prepare"), "dependency is ordered first");

    const kaixa::ExtensionRegistry registry;
    const kaixa::BuildEnvironment environment{workspace.path(), workspace.path() / ".kaixa", "debug"};
    auto plan = kaixa::plan_task(*graph, registry, environment, *preparation);
    context.check(plan.has_value(), "custom task plan is created");
    if (!plan) {
        context.fail(kaixa::format_diagnostic(plan.error()));
        return;
    }
    context.check_equal(plan->action_count(), std::size_t{2}, "one action is created per command");
    context.check_equal(plan->tasks().size(), std::size_t{2}, "custom commands live in the task phase");
    context.check_equal(plan->tasks().front().environment.front().value, std::string("debug"), "environment values are interpolated");

    const auto first = kaixa::execute(*plan);
    context.check(first.has_value(), "custom task closure executes");
    if (!first) {
        context.fail(kaixa::format_diagnostic(first.error()));
        return;
    }
    context.check_equal(first->executed, std::size_t{2}, "both missing outputs are produced");
    context.check(std::filesystem::is_regular_file(workspace.path() / "work/result.txt"), "final output is created");

    const auto second = kaixa::execute(*plan);
    context.check(second.has_value(), "current custom task closure checks successfully");
    if (second)
        context.check_equal(second->executed, std::size_t{0}, "current task outputs skip execution");

    const auto environment_preparation = kaixa::prepare_task(*graph, "environment");
    context.check(environment_preparation.has_value(), "environment task prepares");
    if (!environment_preparation)
        return;

    const auto environment_plan = kaixa::plan_task(*graph, registry, environment, *environment_preparation);
    context.check(environment_plan.has_value(), "environment task plans");
    if (!environment_plan)
        return;

    const auto environment_execution = kaixa::execute(*environment_plan);
    context.check(environment_execution.has_value(), "environment task executes");
    const auto environment_output = kaixa::read_file(workspace.path() / "work/environment.txt");
    context.check(environment_output.has_value(), "environment task writes its output");
    if (environment_output)
        context.check_equal(*environment_output, std::string("debug"), "process environment override reaches the command");
}

KAIXA_TEST(task_preparation_detects_cycles) {
    const kaixa::testing::TempDirectory workspace("task-cycle");
    workspace.write(
        "Kaixa.toml",
        "[package]\n"
        "name = \"tasks\"\n"
        "\n"
        "[command.first]\n"
        "run = [\"cmake\", \"-E\", \"echo\", \"first\"]\n"
        "after = [\"second\"]\n"
        "\n"
        "[command.second]\n"
        "run = [\"cmake\", \"-E\", \"echo\", \"second\"]\n"
        "after = [\"first\"]\n"
    );

    const auto graph = kaixa::load_workspace(workspace.path());
    context.check(graph.has_value(), "cyclic task workspace loads descriptively");
    if (!graph)
        return;

    const auto preparation = kaixa::prepare_task(*graph, "first");
    context.check(!preparation.has_value(), "task dependency cycle fails planning");
    if (!preparation)
        context.check_contains(kaixa::format_diagnostic(preparation.error()), "dependency cycle", "cycle diagnostic is explicit");
}

KAIXA_TEST(task_target_interpolation_requests_the_matching_build) {
    const kaixa::testing::TempDirectory workspace("task-target");
    workspace.write(
        "Kaixa.toml",
        "[package]\n"
        "name = \"app\"\n"
        "resolver = \"cmake\"\n"
        "\n"
        "[bin]\n"
        "sources = [\"main.cpp\"]\n"
        "\n"
        "[[test]]\n"
        "name = \"app.tests\"\n"
        "sources = [\"tests.cpp\"]\n"
        "\n"
        "[command.run-tests-directly]\n"
        "after = [\"build:app.tests\"]\n"
        "run = [\"${target:app.tests}\", \"--direct\"]\n"
        "environment = { BUILD_ROOT = \"${build-dir}\", ARTIFACT = \"${artifact}\" }\n"
    );
    workspace.write("main.cpp", "int main() { return 0; }\n");
    workspace.write("tests.cpp", "int main() { return 0; }\n");

    const auto graph = kaixa::load_workspace(workspace.path());
    context.check(graph.has_value(), "target task workspace loads");
    if (!graph) {
        context.fail(kaixa::format_diagnostic(graph.error()));
        return;
    }

    auto preparation = kaixa::prepare_task(*graph, "run-tests-directly");
    context.check(preparation.has_value(), "target task prepares");
    if (!preparation)
        return;

    context.check(preparation->requires_products, "target interpolation requests product metadata");
    context.check_equal(preparation->build.packages.front().targets.front(), std::string("app.tests"), "matching target is selected");

    const kaixa::PackageId root = graph->roots().front();
    const std::filesystem::path executable = workspace.path() / "expected-tests.exe";
    const std::vector products{
        kaixa::BuildProduct{"app.tests", kaixa::ProductKind::executable, kaixa::ProductPurpose::test, root, executable}
    };
    const kaixa::ExtensionRegistry registry = kaixa::plugin::default_registry();
    const kaixa::BuildEnvironment environment{workspace.path(), workspace.path() / ".kaixa", "debug"};
    const auto plan = kaixa::plan_task(*graph, registry, environment, *preparation, products);
    context.check(plan.has_value(), "target task plan resolves core interpolations");
    if (!plan) {
        context.fail(kaixa::format_diagnostic(plan.error()));
        return;
    }

    const std::span<const kaixa::Action> task_actions = plan->tasks();
    const auto task = task_actions.begin();
    context.check(!task_actions.empty(), "custom action follows the target build plan");
    if (!task_actions.empty()) {
        context.check_equal(task->argv.front(), executable.string(), "target interpolation resolves the product artifact");
        const auto build_root = std::ranges::find(task->environment, std::string("BUILD_ROOT"), &kaixa::EnvironmentVariable::name);
        context.check(
            build_root != task->environment.end() && build_root->value.contains("build"),
            "build directory interpolation resolves a planned output"
        );
        context.check(!task->configured_artifact->empty(), "stable configured artifact is attached to the task");
    }
}

KAIXA_TEST(workflow_preparation_resolves_builtins_and_local_tasks) {
    const kaixa::testing::TempDirectory workspace("workflows");
    workspace.write(
        "Kaixa.toml",
        "[package]\n"
        "name = \"automation\"\n"
        "\n"
        "[command.quality]\n"
        "run = [\"cmake\", \"-E\", \"echo\", \"quality\"]\n"
        "\n"
        "[command.build]\n"
        "run = [\"cmake\", \"-E\", \"echo\", \"custom build\"]\n"
        "\n"
        "[workflow.dev]\n"
        "steps = [\"generate\", \"quality\", \"build\", \"test\", \"task:build\"]\n"
    );

    const auto graph = kaixa::load_workspace(workspace.path());
    context.check(graph.has_value(), "workflow workspace loads");
    if (!graph) {
        context.fail(kaixa::format_diagnostic(graph.error()));
        return;
    }

    const auto preparation = kaixa::prepare_workflow(*graph, "dev");
    context.check(preparation.has_value(), "workflow resolves");
    if (!preparation) {
        context.fail(kaixa::format_diagnostic(preparation.error()));
        return;
    }

    context.check_equal(preparation->steps.size(), std::size_t{5}, "all workflow steps are prepared");
    context.check(preparation->steps[0].kind == kaixa::WorkflowStepKind::generate, "generate is a built-in step");
    context.check(preparation->steps[1].kind == kaixa::WorkflowStepKind::task, "unreserved name resolves a task");
    context.check(preparation->steps[2].kind == kaixa::WorkflowStepKind::build, "reserved build name resolves the built-in");
    context.check(preparation->steps[3].kind == kaixa::WorkflowStepKind::test, "test is a built-in step");
    context.check(preparation->steps[4].kind == kaixa::WorkflowStepKind::task, "task prefix escapes a reserved name");
    context.check_equal(
        preparation->steps[4].task->selected,
        std::string("automation:build"),
        "escaped task resolves relative to the workflow package"
    );
}

KAIXA_TEST(workflow_preparation_reports_unknown_steps) {
    const kaixa::testing::TempDirectory workspace("workflow-errors");
    workspace.write(
        "Kaixa.toml",
        "[package]\n"
        "name = \"automation\"\n"
        "\n"
        "[workflow.invalid]\n"
        "steps = [\"missing\"]\n"
    );

    const auto graph = kaixa::load_workspace(workspace.path());
    context.check(graph.has_value(), "invalid workflow workspace still loads");
    if (!graph)
        return;

    const auto preparation = kaixa::prepare_workflow(*graph, "invalid");
    context.check(!preparation.has_value(), "unknown workflow step fails preparation");
    if (!preparation) {
        const std::string diagnostic = kaixa::format_diagnostic(preparation.error());
        context.check_contains(diagnostic, "unknown command `missing`", "diagnostic identifies the missing task");
        context.check_contains(diagnostic, "workflow `automation:invalid`", "diagnostic identifies the owning workflow");
    }
}

KAIXA_TEST(user_automation_adds_and_overrides_root_declarations) {
    const kaixa::testing::TempDirectory workspace("user-automation");
    workspace.write(
        "Kaixa.toml",
        "[package]\n"
        "name = \"automation\"\n"
        "\n"
        "[command.quality]\n"
        "run = [\"cmake\", \"-E\", \"echo\", \"manifest\"]\n"
    );
    workspace.write(
        "Kaixa.user.toml",
        "[command.quality]\n"
        "run = [\"cmake\", \"-E\", \"echo\", \"user\"]\n"
        "\n"
        "[command.local]\n"
        "run = [\"cmake\", \"-E\", \"echo\", \"local\"]\n"
        "\n"
        "[workflow.local]\n"
        "steps = [\"quality\", \"local\"]\n"
    );

    auto graph = kaixa::load_workspace(workspace.path());
    context.check(graph.has_value(), "workspace for local automation loads");
    if (!graph)
        return;

    auto user = kaixa::parse_configuration_document_file(workspace.path() / "Kaixa.user.toml");
    context.check(user.has_value(), "user automation parses with configuration data");
    if (!user) {
        context.fail(kaixa::format_diagnostic(user.error()));
        return;
    }

    std::vector<kaixa::AutomationDocument> layers;
    layers.push_back(std::move(user->automation));
    const auto applied = kaixa::apply_automation_layers(*graph, std::move(layers));
    context.check(applied.has_value(), "user automation applies to the single root");
    if (!applied) {
        context.fail(kaixa::format_diagnostic(applied.error()));
        return;
    }

    const auto tasks = kaixa::discover_tasks(*graph);
    context.check(tasks.has_value(), "tasks include the user layer");
    if (!tasks)
        return;

    context.check_equal(tasks->size(), std::size_t{2}, "user layer adds one command and replaces one command");
    const auto quality = std::ranges::find_if(*tasks, [](const kaixa::TaskDefinition& task) { return task.declaration.name == "quality"; });
    context.check(quality != tasks->end(), "overridden command remains discoverable");
    if (quality != tasks->end())
        context.check_equal(quality->declaration.run.back(), std::string("user"), "user command replaces the manifest command");

    const auto workflow = kaixa::prepare_workflow(*graph, "local");
    context.check(workflow.has_value(), "user workflow resolves its local commands");
    if (workflow)
        context.check_equal(workflow->steps.size(), std::size_t{2}, "user workflow retains both steps");
}

KAIXA_TEST(user_automation_requires_package_scope_for_multiple_roots) {
    const kaixa::testing::TempDirectory workspace("scoped-user-automation");
    workspace.write(
        "Kaixa.toml",
        "[package-set]\n"
        "members = [\"packages/*\"]\n"
        "default = [\"editor\", \"game\"]\n"
    );
    workspace.write("packages/editor/Kaixa.toml", "[package]\nname = \"editor\"\nresolver = \"cmake\"\n");
    workspace.write("packages/game/Kaixa.toml", "[package]\nname = \"game\"\nresolver = \"cmake\"\n");
    workspace.write("unscoped.toml", "[command.local]\nrun = [\"cmake\", \"-E\", \"echo\"]\n");
    workspace.write(
        "scoped.toml",
        "[command.local]\n"
        "package = \"game\"\n"
        "run = [\"cmake\", \"-E\", \"echo\"]\n"
        "\n"
        "[workflow.local]\n"
        "package = \"game\"\n"
        "steps = [\"local\"]\n"
    );

    auto graph = kaixa::load_workspace(workspace.path());
    context.check(graph.has_value(), "multi-root automation workspace loads");
    if (!graph)
        return;

    auto unscoped = kaixa::parse_configuration_document_file(workspace.path() / "unscoped.toml");
    context.check(unscoped.has_value(), "unscoped user command parses before graph application");
    if (!unscoped)
        return;

    std::vector<kaixa::AutomationDocument> unscoped_layers;
    unscoped_layers.push_back(std::move(unscoped->automation));
    const auto rejected = kaixa::apply_automation_layers(*graph, std::move(unscoped_layers));
    context.check(!rejected.has_value(), "unscoped user command is ambiguous across roots");
    if (!rejected) {
        context.check_contains(
            kaixa::format_diagnostic(rejected.error()),
            "requires `package`",
            "multi-root diagnostic requests package scope"
        );
    }

    auto scoped = kaixa::parse_configuration_document_file(workspace.path() / "scoped.toml");
    context.check(scoped.has_value(), "package-scoped user automation parses");
    if (!scoped)
        return;

    std::vector<kaixa::AutomationDocument> scoped_layers;
    scoped_layers.push_back(std::move(scoped->automation));
    const auto applied = kaixa::apply_automation_layers(*graph, std::move(scoped_layers));
    context.check(applied.has_value(), "package-scoped user automation applies");
    if (!applied)
        context.fail(kaixa::format_diagnostic(applied.error()));

    const auto tasks = kaixa::discover_tasks(*graph);
    context.check(tasks.has_value() && tasks->size() == 1, "only the selected package receives the local command");
    if (tasks && !tasks->empty())
        context.check_equal(tasks->front().qualified_name, std::string("game:local"), "local command keeps package qualification");
}
