#include <test_support.hpp>

#include <kaixa/kaixa.hpp>
#include <kaixa/plugin/bundle.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>

namespace {
    const std::filesystem::path workspaces_directory =
        std::filesystem::path(KAIXA_TESTS_DIR) / "workspaces";
}

KAIXA_TEST(single_package_workspace_loads_and_plans) {
    const std::filesystem::path workspace = workspaces_directory / "single_package";
    const auto graph = kaixa::load_workspace(workspace);
    context.check(graph.has_value(), "test workspace loads");
    if (!graph) {
        context.fail(kaixa::format_diagnostic(graph.error()));
        return;
    }

    context.check_equal(graph->size(), std::size_t{1}, "one package");
    context.check_equal((*graph)[graph->root()].name, std::string("test_single"), "package name");

    const kaixa::ResolverRegistry registry = kaixa::plugin::default_registry();
    const kaixa::BuildEnvironment environment{
        workspace,
        workspace / ".test-output",
        "debug"
    };
    const auto plan = kaixa::plan_build(*graph, registry, environment);
    context.check(plan.has_value(), "test workspace plans");
    if (plan) {
        context.check_equal(plan->actions().size(), std::size_t{2}, "configure and build");
        context.check_equal(
            plan->actions().front().description,
            std::string("configure test_single"),
            "configure action"
        );
    }
}

KAIXA_TEST(source_dependency_workspace_models_managed_and_opaque_packages) {
    const std::filesystem::path workspace = workspaces_directory / "source_dependency";
    const auto graph = kaixa::load_workspace(workspace);
    context.check(graph.has_value(), "test workspace loads");
    if (!graph) {
        context.fail(kaixa::format_diagnostic(graph.error()));
        return;
    }

    context.check_equal(graph->size(), std::size_t{3}, "three packages");
    const auto math = graph->find_by_name("test_source_math");
    const auto assets = graph->find_by_name("assets");
    context.check(math.has_value(), "managed dependency exists");
    context.check(assets.has_value(), "opaque dependency exists");
    if (assets)
        context.check((*graph)[*assets].kind == kaixa::PackageKind::opaque, "assets are opaque");

    const auto order = graph->build_order();
    context.check(order.has_value(), "workspace has an order");
    if (order && math) {
        const auto math_position = std::ranges::find(*order, *math);
        const auto root_position = std::ranges::find(*order, graph->root());
        context.check(math_position < root_position, "managed dependency precedes root");
    }

    const kaixa::ResolverRegistry registry = kaixa::plugin::default_registry();
    const kaixa::BuildEnvironment environment{
        workspace,
        workspace / ".test-output",
        "debug"
    };
    const auto plan = kaixa::plan_build(*graph, registry, environment);
    context.check(plan.has_value(), "workspace example plans");
    if (plan) {
        context.check_equal(plan->actions().size(), std::size_t{2}, "one composed CMake build");
        context.check_equal(plan->generated_files().size(), std::size_t{1}, "integration file");
        if (plan->actions().size() == 2 && !plan->generated_files().empty()) {
            context.check_equal(
                plan->actions()[0].description,
                std::string("configure test_source_app"),
                "root is configured once"
            );
            context.check_contains(
                plan->generated_files().front().content,
                "add_subdirectory",
                "source dependency is composed"
            );
            const auto& configure = plan->actions()[0].argv;
            context.check(
                std::ranges::find_if(configure, [](const std::string& argument) {
                    return argument.starts_with("-DCMAKE_PROJECT_INCLUDE=");
                }) != configure.end(),
                "root receives the generated integration file"
            );
        }
    }
}

KAIXA_TEST(package_dependency_workspace_uses_install_and_find_package) {
    const std::filesystem::path workspace = workspaces_directory / "package_dependency";
    const auto graph = kaixa::load_workspace(workspace);
    if (!graph) {
        context.fail(kaixa::format_diagnostic(graph.error()));
        return;
    }

    const kaixa::ResolverRegistry registry = kaixa::plugin::default_registry();
    const kaixa::BuildEnvironment environment{workspace, workspace / ".test-output", "debug"};
    const auto plan = kaixa::plan_build(*graph, registry, environment);
    context.check(plan.has_value(), "package dependency example plans");
    if (!plan)
        return;

    context.check_equal(plan->actions().size(), std::size_t{5}, "dependency installs before app");
    if (plan->actions().size() != 5)
        return;

    context.check_equal(
        plan->actions()[2].description,
        std::string("install test_package_math"),
        "provider is installed"
    );
    context.check_equal(
        plan->actions()[3].description,
        std::string("configure test_package_app"),
        "consumer configures after provider"
    );

    const std::string expected_prefix = "-DKAIXA_CMAKE_PREFIX_PATH="
        + (environment.state_root / "artifacts" / "cmake" / "test_package_math").string();
    const auto& configure = plan->actions()[3].argv;
    context.check(
        std::ranges::find(configure, expected_prefix) != configure.end(),
        "consumer receives the package prefix"
    );
}
