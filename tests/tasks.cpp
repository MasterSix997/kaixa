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
        "[[command]]\n"
        "name = \"direct\"\n"
        "run = [\"cmake\", \"-E\", \"echo\", \"ok\"]\n"
        "working-directory = \"tools\"\n"
        "inputs = [\"input.txt\"]\n"
        "outputs = [\"output.txt\"]\n"
        "after = [\"prepare\"]\n"
        "environment = { MODE = \"strict\" }\n"
        "\n"
        "[[command]]\n"
        "name = \"prepare\"\n"
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
        "[[command]]\n"
        "name = \"invalid\"\n"
        "run = [\"cmake\", \"-E\", \"echo\"]\n"
        "tool = \"powershell\"\n"
        "script = \"Invalid.ps1\"\n",
        "commands.toml"
    );
    context.check(!manifest.has_value(), "ambiguous invocation fails during manifest parsing");
    if (!manifest)
        context.check_contains(kaixa::format_diagnostic(manifest.error()), "cannot combine", "diagnostic explains the exclusive forms");
}

KAIXA_TEST(task_planning_orders_dependencies_and_executes_incrementally) {
    const kaixa::testing::TempDirectory workspace("custom-tasks");
    workspace.write(
        "Kaixa.toml",
        "[package]\n"
        "name = \"tasks\"\n"
        "\n"
        "[[command]]\n"
        "name = \"prepare\"\n"
        "run = [\"cmake\", \"-E\", \"copy\", \"input.txt\", \"prepared.txt\"]\n"
        "working-directory = \"work\"\n"
        "inputs = [\"input.txt\"]\n"
        "outputs = [\"prepared.txt\"]\n"
        "environment = { KAIXA_TASK_PROFILE = \"${profile}\" }\n"
        "\n"
        "[[command]]\n"
        "name = \"finish\"\n"
        "run = [\"cmake\", \"-E\", \"copy\", \"prepared.txt\", \"result.txt\"]\n"
        "working-directory = \"work\"\n"
        "inputs = [\"prepared.txt\"]\n"
        "outputs = [\"result.txt\"]\n"
        "after = [\"prepare\"]\n"
        "\n"
        "[[command]]\n"
        "name = \"environment\"\n"
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
    context.check_equal(plan->actions().size(), std::size_t{2}, "one action is created per command");
    context.check(plan->actions().front().stage == kaixa::ActionStage::task, "custom command has the task stage");
    context.check_equal(plan->actions().front().environment.front().value, std::string("debug"), "environment values are interpolated");

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
        "[[command]]\n"
        "name = \"first\"\n"
        "run = [\"cmake\", \"-E\", \"echo\", \"first\"]\n"
        "after = [\"second\"]\n"
        "\n"
        "[[command]]\n"
        "name = \"second\"\n"
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
        "[[command]]\n"
        "name = \"run-tests-directly\"\n"
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

    const auto task = std::ranges::find(plan->actions(), kaixa::ActionStage::task, &kaixa::Action::stage);
    context.check(task != plan->actions().end(), "custom action follows the target build plan");
    if (task != plan->actions().end()) {
        context.check_equal(task->argv.front(), executable.string(), "target interpolation resolves the product artifact");
        const auto build_root = std::ranges::find(task->environment, std::string("BUILD_ROOT"), &kaixa::EnvironmentVariable::name);
        context.check(
            build_root != task->environment.end() && build_root->value.contains("build"),
            "build directory interpolation resolves a planned output"
        );
        context.check(!task->configured_artifact->empty(), "stable configured artifact is attached to the task");
    }
}
