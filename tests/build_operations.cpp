#include <test_support.hpp>

#include <kaixa/kaixa.hpp>
#include <kaixa/plugin/bundle.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <utility>

using kaixa::testing::TempDirectory;

KAIXA_TEST(check_is_read_only_and_reports_missing_outputs) {
    const TempDirectory root("check-plan");
    const std::filesystem::path generated = root.path() / "generated/CMakeLists.txt";
    const std::filesystem::path input = root.path() / "input.txt";
    const std::filesystem::path output = root.path() / "output.txt";
    root.write("input.txt", "input\n");

    kaixa::ExecutionPlan plan;
    plan.generate({generated, "generated\n"});
    kaixa::Action configure{"configure", {"unused"}, root.path(), {input}, {output}};
    plan.synchronize(std::move(configure));

    const auto report = kaixa::check(plan);
    context.check(report.has_value(), "plan can be checked");
    if (!report)
        return;

    context.check_equal(report->generated_files.size(), std::size_t{1}, "one generated file");
    context.check(report->generated_files.front().state == kaixa::GeneratedFileState::missing, "missing generated file is reported");
    context.check(report->synchronization.front().state == kaixa::ActionState::required, "missing action output is required");
    context.check_equal(report->synchronization.size(), std::size_t{1}, "the action stays in the synchronization phase");
    context.check(report->requires_synchronization(), "report requires synchronization");
    context.check(!std::filesystem::exists(generated.parent_path()), "check creates no directory");
}

KAIXA_TEST(generate_writes_only_changed_files) {
    const TempDirectory root("generate-plan");
    const std::filesystem::path generated = root.path() / "generated.txt";

    kaixa::ExecutionPlan plan;
    plan.generate({generated, "first\n"});

    const auto first = kaixa::generate(plan);
    context.check(first.has_value(), "missing file is generated");
    if (!first)
        return;

    context.check_equal(first->written, std::size_t{1}, "one file written");
    context.check_equal(first->unchanged, std::size_t{0}, "no unchanged file initially");
    context.check_equal(first->synchronized, std::size_t{0}, "no synchronization action");

    const auto before = std::filesystem::last_write_time(generated);
    const auto second = kaixa::generate(plan);
    context.check(second.has_value(), "current file is accepted");
    if (!second)
        return;

    context.check_equal(second->written, std::size_t{0}, "current file is not rewritten");
    context.check_equal(second->unchanged, std::size_t{1}, "current file counted");
    context.check(std::filesystem::last_write_time(generated) == before, "current file keeps its timestamp");

    plan = kaixa::ExecutionPlan{};
    plan.generate({generated, "second\n"});
    const auto changed = kaixa::check(plan);
    context.check(changed.has_value(), "changed file can be checked");
    if (changed) {
        context.check(changed->generated_files.front().state == kaixa::GeneratedFileState::different, "different content is reported");
    }
}

KAIXA_TEST(check_distinguishes_current_and_unknown_actions) {
    const TempDirectory root("check-actions");
    const std::filesystem::path input = root.path() / "input.txt";
    const std::filesystem::path output = root.path() / "output.txt";
    const std::filesystem::path output_directory = root.path() / "build";
    root.write("input.txt", "input\n");
    root.write("output.txt", "output\n");
    std::filesystem::create_directories(output_directory);

    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(input, now - std::chrono::seconds(2));
    std::filesystem::last_write_time(output, now);

    kaixa::ExecutionPlan plan;
    plan.synchronize({"configure", {"unused"}, root.path(), {input}, {output}});
    plan.build({"build", {"unused"}, root.path(), {output}, {output_directory}});

    auto report = kaixa::check(plan);
    context.check(report.has_value(), "action states can be checked");
    if (!report)
        return;

    context.check(report->synchronization[0].state == kaixa::ActionState::current, "newer regular output is current");
    context.check(report->build[0].state == kaixa::ActionState::unknown, "directory output has backend-owned state");
    context.check(!report->requires_synchronization(), "unknown is not reported as required");

    std::filesystem::last_write_time(input, now + std::chrono::seconds(2));
    report = kaixa::check(plan);
    context.check(report.has_value(), "stale action can be checked");
    if (report) {
        context.check(report->synchronization[0].state == kaixa::ActionState::unknown, "newer input leaves backend state unknown");
        context.check(!report->requires_synchronization(), "unknown action is not reported as required");
    }
}

KAIXA_TEST(generate_executes_synchronization_without_building) {
    const TempDirectory root("generate-actions");
    const std::filesystem::path synchronized = root.path() / "synchronized.txt";
    const std::filesystem::path built = root.path() / "built.txt";

    kaixa::Action synchronize;
    synchronize.description = "synchronize";
    synchronize.argv = {"cmake", "-E", "touch", synchronized.string()};
    synchronize.working_directory = root.path();
    synchronize.outputs.push_back(synchronized);

    kaixa::Action build;
    build.description = "build";
    build.argv = {"cmake", "-E", "touch", built.string()};
    build.working_directory = root.path();
    build.outputs.push_back(built);

    kaixa::ExecutionPlan plan;
    plan.synchronize(std::move(synchronize));
    plan.build(std::move(build));

    const auto generated = kaixa::generate(plan);
    context.check(generated.has_value(), "plan synchronizes");
    if (!generated)
        return;

    context.check_equal(generated->synchronized, std::size_t{1}, "one synchronized action");
    context.check(std::filesystem::exists(synchronized), "synchronization action executes");
    context.check(!std::filesystem::exists(built), "build action does not execute");

    const auto executed = kaixa::execute(plan);
    context.check(executed.has_value(), "complete plan executes");
    if (executed) {
        context.check_equal(executed->executed, std::size_t{1}, "current synchronization is skipped");
        context.check(std::filesystem::exists(built), "build action executes during build");
    }
}

KAIXA_TEST(changed_generated_input_requires_synchronization) {
    const TempDirectory root("generated-input-sync");
    const std::filesystem::path generated = root.path() / "CMakeLists.txt";
    const std::filesystem::path configured = root.path() / "configured.txt";

    kaixa::ExecutionPlan plan;
    plan.generate({generated, "generated\n"});

    kaixa::Action configure;
    configure.description = "configure";
    configure.argv = {"cmake", "-E", "touch", configured.string()};
    configure.working_directory = root.path();
    configure.inputs.push_back(generated);
    configure.outputs.push_back(configured);
    configure.checked_state = kaixa::ActionState::current;
    plan.synchronize(std::move(configure));

    const auto state = kaixa::check(plan);
    context.check(state.has_value(), "generated input plan can be checked");
    if (state) {
        context.check(
            state->synchronization.front().state == kaixa::ActionState::required,
            "changed generated input promotes synchronization"
        );
    }

    const auto report = kaixa::generate(plan);
    context.check(report.has_value(), "generated input synchronizes");
    if (!report)
        return;

    context.check_equal(report->synchronized, std::size_t{1}, "one action synchronizes");
    context.check(std::filesystem::exists(configured), "synchronization action executes");
}

KAIXA_TEST(test_planning_can_include_selected_dependency_packages_without_promoting_graph_roots) {
    const TempDirectory root("dependency-test-planning");
    root.write(
        "Kaixa.toml",
        "[package]\n"
        "name = \"application\"\n"
        "version = \"1.0.0\"\n"
        "resolver = \"cmake\"\n"
        "\n"
        "[dependencies]\n"
        "dependency = { path = \"dependency\" }\n"
        "\n"
        "[lib]\n"
        "type = \"interface\"\n"
    );
    root.write("dependency/dependency.test.cpp", "int main() { return 0; }\n");
    root.write(
        "dependency/Kaixa.toml",
        "[package]\n"
        "name = \"dependency\"\n"
        "version = \"1.0.0\"\n"
        "resolver = \"cmake\"\n"
        "\n"
        "[lib]\n"
        "type = \"interface\"\n"
        "\n"
        "[[test]]\n"
        "name = \"dependency.tests\"\n"
        "sources = [\"dependency.test.cpp\"]\n"
    );

    const auto graph = kaixa::load_workspace(root.path());
    context.check(graph.has_value(), "dependency workspace loads");
    if (!graph)
        return;

    const auto application = graph->find_by_name("application");
    const auto dependency = graph->find_by_name("dependency");
    context.check(application.has_value() && dependency.has_value(), "root and dependency are resolved");
    if (!application || !dependency)
        return;

    kaixa::TestRequest request;
    request.packages = {*application, *dependency};
    const kaixa::ExtensionRegistry registry = kaixa::plugin::default_registry();
    const kaixa::BuildEnvironment environment{root.path(), root.path() / ".kaixa", "debug"};
    const auto plan = kaixa::plan_tests(*graph, registry, environment, request);
    context.check(plan.has_value(), "dependency test selection plans");
    if (!plan)
        return;

    context.check_equal(plan->tests().size(), std::size_t{1}, "only packages with tests receive test actions");
    if (plan->tests().size() == 1)
        context.check(plan->tests()[0].package == dependency, "dependency is selected for tests");
    context.check(
        std::ranges::any_of(plan->builds(), [&](const kaixa::Action& action) { return action.package == dependency; }),
        "dependency tests receive their own build action"
    );
    context.check_equal(graph->roots().size(), std::size_t{1}, "dependency is not promoted to a resolution root");
}

KAIXA_TEST(execution_reports_captured_action_output) {
    kaixa::ExecutionPlan plan;
    kaixa::Action action;
    action.description = "captured action";
    action.argv = {"cmake", "-E", "echo", "captured text"};
    action.output = kaixa::ProcessOutputMode::capture;
    plan.build(std::move(action));

    const auto report = kaixa::execute(plan);
    context.check(report.has_value(), "captured action executes");
    if (!report)
        return;

    context.check_equal(report->captured_outputs.size(), std::size_t{1}, "one captured output is reported");
    if (!report->captured_outputs.empty()) {
        context.check_equal(report->captured_outputs.front().description, std::string("captured action"), "action identity is retained");
        context.check_contains(report->captured_outputs.front().content, "captured text", "captured output content is retained");
    }
}
