#include <test_support.hpp>

#include <kaixa/kaixa.hpp>

#include <chrono>
#include <filesystem>

using kaixa::testing::TempDirectory;

KAIXA_TEST(check_is_read_only_and_reports_missing_outputs) {
    const TempDirectory root("check-plan");
    const std::filesystem::path generated = root.path() / "generated/CMakeLists.txt";
    const std::filesystem::path input = root.path() / "input.txt";
    const std::filesystem::path output = root.path() / "output.txt";
    root.write("input.txt", "input\n");

    kaixa::BuildPlan plan;
    plan.generate({generated, "generated\n"});
    plan.add({"configure", {"unused"}, root.path(), {input}, {output}});

    const auto report = kaixa::check(plan);
    context.check(report.has_value(), "plan can be checked");
    if (!report)
        return;

    context.check_equal(report->generated_files.size(), std::size_t{1}, "one generated file");
    context.check(
        report->generated_files.front().state == kaixa::GeneratedFileState::missing,
        "missing generated file is reported"
    );
    context.check(
        report->actions.front().state == kaixa::ActionState::required,
        "missing action output is required"
    );
    context.check(report->requires_action(), "report requires action");
    context.check(!std::filesystem::exists(generated.parent_path()), "check creates no directory");
}

KAIXA_TEST(generate_writes_only_changed_files) {
    const TempDirectory root("generate-plan");
    const std::filesystem::path generated = root.path() / "generated.txt";

    kaixa::BuildPlan plan;
    plan.generate({generated, "first\n"});

    const auto first = kaixa::generate(plan);
    context.check(first.has_value(), "missing file is generated");
    if (!first)
        return;
    context.check_equal(first->written, std::size_t{1}, "one file written");
    context.check_equal(first->unchanged, std::size_t{0}, "no unchanged file initially");

    const auto before = std::filesystem::last_write_time(generated);
    const auto second = kaixa::generate(plan);
    context.check(second.has_value(), "current file is accepted");
    if (!second)
        return;
    context.check_equal(second->written, std::size_t{0}, "current file is not rewritten");
    context.check_equal(second->unchanged, std::size_t{1}, "current file counted");
    context.check(
        std::filesystem::last_write_time(generated) == before,
        "current file keeps its timestamp"
    );

    plan = kaixa::BuildPlan{};
    plan.generate({generated, "second\n"});
    const auto changed = kaixa::check(plan);
    context.check(changed.has_value(), "changed file can be checked");
    if (changed) {
        context.check(
            changed->generated_files.front().state == kaixa::GeneratedFileState::different,
            "different content is reported"
        );
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

    kaixa::BuildPlan plan;
    plan.add({"configure", {"unused"}, root.path(), {input}, {output}});
    plan.add({"build", {"unused"}, root.path(), {output}, {output_directory}});

    auto report = kaixa::check(plan);
    context.check(report.has_value(), "action states can be checked");
    if (!report)
        return;
    context.check(
        report->actions[0].state == kaixa::ActionState::current,
        "newer regular output is current"
    );
    context.check(
        report->actions[1].state == kaixa::ActionState::unknown,
        "directory output has backend-owned state"
    );
    context.check(!report->requires_action(), "unknown is not reported as required");

    std::filesystem::last_write_time(input, now + std::chrono::seconds(2));
    report = kaixa::check(plan);
    context.check(report.has_value(), "stale action can be checked");
    if (report) {
        context.check(
            report->actions[0].state == kaixa::ActionState::unknown,
            "newer input leaves backend state unknown"
        );
        context.check(!report->requires_action(), "unknown action is not reported as required");
    }
}
