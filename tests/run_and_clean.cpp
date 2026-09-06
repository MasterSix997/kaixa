#include <test_support.hpp>

#include <kaixa/kaixa.hpp>

#include <array>
#include <filesystem>
#include <string>
#include <vector>

using kaixa::testing::TempDirectory;

KAIXA_TEST(package_inspection_expands_shared_dependencies_once) {
    kaixa::Graph graph;
    const auto add_package = [&](std::string name) {
        kaixa::PackageNode package;
        package.name = std::move(name);
        package.directory = package.name;
        package.resolver = "cmake";
        package.semantics = kaixa::ManagedPackage{kaixa::Manifest{package.name, "cmake"}};
        return graph.add(std::move(package));
    };

    const kaixa::PackageId root = add_package("root");
    const kaixa::PackageId left = add_package("left");
    const kaixa::PackageId right = add_package("right");
    const kaixa::PackageId shared = add_package("shared");
    const kaixa::PackageId leaf = add_package("leaf");
    const kaixa::PackageId test_support = add_package("test_support");
    graph[root].dependencies = {left, right};
    graph[root].target_dependencies = {{"root_tests", kaixa::PackageTargetKind::test, {test_support}}};
    graph[left].dependencies = {shared};
    graph[right].dependencies = {shared};
    graph[shared].dependencies = {leaf};
    graph.add_root(root);

    const std::vector<kaixa::PackageDependencyEntry> tree = graph.dependency_tree(graph.roots());
    context.check_equal(tree.size(), std::size_t{7}, "package and target dependency edges are visible");
    if (tree.size() == 7) {
        context.check(tree[2].package == shared && !tree[2].repeated, "shared dependency is expanded on first use");
        context.check(tree[3].package == leaf, "first shared dependency expands its subtree");
        context.check(tree[5].package == shared && tree[5].repeated, "later shared dependency is a reference only");
        context.check(tree[6].package == test_support, "target-only dependency is included in the package tree");
    }
}

KAIXA_TEST(run_target_selection_prefers_the_package_name) {
    const std::array targets = {kaixa::RunTarget{"tools", kaixa::ProductPurpose::primary, {{"tools"}, {}}},
        kaixa::RunTarget{"app", kaixa::ProductPurpose::primary, {{"app"}, {}}}};

    const auto selected = kaixa::select_run_target(targets, std::nullopt, "app");
    context.check(selected.has_value(), "package target is selected");
    if (selected)
        context.check_equal(selected->name, std::string("app"), "selected package target");
}

KAIXA_TEST(run_target_selection_requires_a_choice_when_ambiguous) {
    const std::array targets = {kaixa::RunTarget{"editor", kaixa::ProductPurpose::primary, {{"editor"}, {}}},
        kaixa::RunTarget{"game", kaixa::ProductPurpose::primary, {{"game"}, {}}}};

    const auto selected = kaixa::select_run_target(targets, std::nullopt, "workspace");
    context.check(!selected.has_value(), "ambiguous targets are rejected");
    if (!selected) {
        context.check_contains(kaixa::format_diagnostic(selected.error()), "--target", "ambiguity explains target selection");
    }
}

KAIXA_TEST(run_target_selection_lists_available_targets_for_an_unknown_name) {
    const std::array targets = {kaixa::RunTarget{"editor", kaixa::ProductPurpose::primary, {{"editor"}, {}}},
        kaixa::RunTarget{"game", kaixa::ProductPurpose::primary, {{"game"}, {}}}};

    const auto selected = kaixa::select_run_target(targets, std::string("server"), "workspace");
    context.check(!selected.has_value(), "unknown target is rejected");
    if (!selected) {
        const std::string diagnostic = kaixa::format_diagnostic(selected.error());
        context.check_contains(diagnostic, "server", "diagnostic names requested target");
        context.check_contains(diagnostic, "editor, game", "diagnostic lists available targets");
    }
}

KAIXA_TEST(run_target_selection_rejects_duplicate_names_across_packages) {
    const std::array targets = {kaixa::RunTarget{"app", kaixa::ProductPurpose::primary, {{"first"}, {}}},
        kaixa::RunTarget{"app", kaixa::ProductPurpose::primary, {{"second"}, {}}}};

    const auto selected = kaixa::select_run_target(targets, std::string("app"), {});
    context.check(!selected.has_value(), "duplicate runnable names are rejected");
    if (!selected) {
        context.check_contains(kaixa::format_diagnostic(selected.error()), "--package", "diagnostic explains how to narrow the package");
    }
}

KAIXA_TEST(clean_removes_only_planned_state) {
    const TempDirectory root("clean-plan");
    const std::filesystem::path state = root.path() / ".kaixa";
    const std::filesystem::path debug = state / "build/debug";
    const std::filesystem::path release = state / "build/release";
    root.write(".kaixa/build/debug/app.txt", "debug\n");
    root.write(".kaixa/build/release/app.txt", "release\n");

    kaixa::CleanPlan plan;
    plan.add(debug);
    const auto dry_run = kaixa::clean(plan, state, root.path(), true);
    context.check(dry_run.has_value(), "clean dry run succeeds");
    context.check(std::filesystem::exists(debug), "dry run preserves selected state");

    const auto report = kaixa::clean(plan, state, root.path());
    context.check(report.has_value(), "clean succeeds");
    context.check(!std::filesystem::exists(debug), "selected state is removed");
    context.check(std::filesystem::exists(release), "other configuration is preserved");
}

KAIXA_TEST(clean_rejects_paths_outside_kaixa_state) {
    const TempDirectory root("clean-safety");
    const std::filesystem::path state = root.path() / ".kaixa";
    root.write("outside.txt", "keep\n");

    kaixa::CleanPlan plan;
    plan.add(root.path() / "outside.txt");
    const auto report = kaixa::clean(plan, state, root.path());
    context.check(!report.has_value(), "outside clean path is rejected");
    context.check(std::filesystem::exists(root.path() / "outside.txt"), "outside file remains");
}

KAIXA_TEST(clean_all_removes_only_the_state_root) {
    const TempDirectory root("clean-all");
    const std::filesystem::path state = root.path() / ".kaixa";
    root.write("Kaixa.toml", "[package]\nname = \"app\"\nresolver = \"cmake\"\n");
    root.write(".kaixa/build/debug/app.txt", "debug\n");

    kaixa::CleanPlan plan;
    plan.add(state);
    const auto report = kaixa::clean(plan, state, root.path(), false, true);
    context.check(report.has_value(), "clean all succeeds");
    context.check(!std::filesystem::exists(state), "state root is removed");
    context.check(std::filesystem::exists(root.path() / "Kaixa.toml"), "manifest remains");
}

KAIXA_TEST(clean_removes_signed_generated_files_only_when_planned) {
    const TempDirectory root("clean-generated");
    const std::filesystem::path state = root.path() / ".kaixa";
    const std::filesystem::path generated = root.path() / "CMakeLists.txt";
    root.write("CMakeLists.txt", "# Generated by Kaixa.\nproject(app)\n");

    kaixa::CleanPlan regular;
    regular.add(state / "build/debug");
    const auto regular_report = kaixa::clean(regular, state, root.path());
    context.check(regular_report.has_value(), "regular clean succeeds");
    context.check(std::filesystem::exists(generated), "regular clean preserves generation");

    kaixa::CleanPlan with_generation;
    with_generation.generated_file({generated, "# Generated by Kaixa."});
    const auto generated_report = kaixa::clean(with_generation, state, root.path());
    context.check(generated_report.has_value(), "generated clean succeeds");
    if (!generated_report)
        context.fail(kaixa::format_diagnostic(generated_report.error()));

    context.check(!std::filesystem::exists(generated), "signed generated file is removed");
}

KAIXA_TEST(clean_refuses_unsigned_generated_files) {
    const TempDirectory root("clean-generated-safety");
    const std::filesystem::path state = root.path() / ".kaixa";
    const std::filesystem::path manual = root.path() / "CMakeLists.txt";
    root.write("CMakeLists.txt", "project(manual)\n");
    root.write(".kaixa/build/debug/app.txt", "build\n");

    kaixa::CleanPlan plan;
    plan.add(state / "build/debug");
    plan.generated_file({manual, "# Generated by Kaixa."});
    const auto report = kaixa::clean(plan, state, root.path());
    context.check(!report.has_value(), "manual generated candidate is rejected");
    context.check(std::filesystem::exists(manual), "manual file remains");
    context.check(std::filesystem::exists(state / "build/debug/app.txt"), "validation happens before state removal");
}
