#include <test_support.hpp>

#include <kaixa/kaixa.hpp>
#include <kaixa/plugin/bundle.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace {
    const std::filesystem::path workspaces_directory =
        std::filesystem::path(__FILE__).parent_path() / "workspaces";
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
            const std::vector<std::string> configure = plan->actions()[0].argv;
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

    const std::string artifact_root =
        (environment.state_root / "artifacts" / "cmake").string();
    const std::vector<std::string> configure = plan->actions()[3].argv;
    context.check(
        std::ranges::find_if(configure, [&](const std::string& argument) {
            return argument.starts_with("-DKAIXA_CMAKE_PREFIX_PATH=")
                && argument.contains(artifact_root)
                && argument.ends_with("test_package_math");
        }) != configure.end(),
        "consumer receives the package prefix"
    );
}

KAIXA_TEST(generated_project_workspace_builds_from_kaixa_toml) {
    const kaixa::testing::TempDirectory workspace("generated-cmake-project");
    workspace.copy_from(workspaces_directory / "generated_project");

    const auto graph = kaixa::load_workspace(workspace.path());
    if (!graph) {
        context.fail(kaixa::format_diagnostic(graph.error()));
        return;
    }

    const kaixa::ResolverRegistry registry = kaixa::plugin::default_registry();
    const kaixa::BuildEnvironment environment{
        workspace.path(),
        workspace.path() / ".kaixa",
        "debug"
    };
    const auto plan = kaixa::plan_build(*graph, registry, environment);
    context.check(plan.has_value(), "generated project plans");
    if (!plan)
        return;

    const auto generated = std::ranges::find_if(
        plan->generated_files(),
        [](const kaixa::GeneratedFile& candidate) {
            return candidate.path.filename() == "CMakeLists.txt"
                && candidate.path.parent_path().filename() == "project"
                && candidate.path.parent_path().parent_path().filename() == "test_generated";
        }
    );
    context.check(
        generated != plan->generated_files().end(),
        "CMakeLists.txt is generated in Kaixa state"
    );
    context.check(
        !std::filesystem::exists(workspace.path() / "CMakeLists.txt"),
        "source root remains clean"
    );
    context.check(
        !std::filesystem::exists(workspace.path() / "packages/math/CMakeLists.txt"),
        "dependency source remains clean"
    );
    if (generated != plan->generated_files().end()) {
        context.check_contains(
            generated->content,
            "add_library(test_generated_support STATIC",
            "library target command"
        );
        context.check_contains(
            generated->content,
            "add_executable(test_generated",
            "executable target command"
        );
        context.check_contains(
            generated->content,
            "SYSTEM PUBLIC",
            "system include scope"
        );
        context.check_contains(
            generated->content,
            "target_compile_definitions",
            "compile definitions"
        );
        context.check_contains(generated->content, "add_test", "CTest registration");
        context.check_contains(generated->content, "cxx_std_23", "C++ standard");
    }

    const auto report = kaixa::execute(*plan);
    context.check(report.has_value(), "generated project configures and builds");
    if (!report)
        context.fail(kaixa::format_diagnostic(report.error()));
    else
        context.check_equal(report->executed, std::size_t{2}, "configure and build execute");
}

KAIXA_TEST(generated_project_refuses_to_replace_a_manual_cmakelists) {
    const kaixa::testing::TempDirectory workspace("manual-cmakelists");
    kaixa::Manifest manifest{"manual", "cmake"};
    manifest.resolver_options = kaixa::Value::table({
        {"target", kaixa::Value::table({
            {"type", "executable"},
            {"sources", kaixa::Value::array({"main.cpp"})}
        })}
    });
    workspace.write_manifest("Kaixa.toml", manifest);
    workspace.write("main.cpp", "int main() { return 0; }\n");
    workspace.write("CMakeLists.txt", "# This file belongs to the user.\n");

    const auto graph = kaixa::load_workspace(workspace.path());
    if (!graph) {
        context.fail(kaixa::format_diagnostic(graph.error()));
        return;
    }

    const kaixa::ResolverRegistry registry = kaixa::plugin::default_registry();
    const kaixa::BuildEnvironment environment{
        workspace.path(),
        workspace.path() / ".kaixa",
        "debug"
    };
    const auto plan = kaixa::plan_build(*graph, registry, environment);
    context.check(!plan.has_value(), "manual CMakeLists.txt is protected");
    if (!plan) {
        context.check_contains(
            kaixa::format_diagnostic(plan.error()),
            "was not generated by Kaixa",
            "diagnostic explains the ownership rule"
        );
    }
}
