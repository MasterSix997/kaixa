#include <test_support.hpp>

#include <kaixa/kaixa.hpp>
#include <kaixa/plugin/bundle.hpp>

#include <algorithm>
#include <filesystem>
#include <string>

KAIXA_TEST(test_adapters_normalize_framework_cases_and_invocations) {
    const auto googletest = kaixa::test_adapter("googletest", kaixa::PackageTargetKind::test);
    context.check(googletest.has_value(), "GoogleTest adapter is available");
    if (googletest) {
        const auto cases = kaixa::parse_test_cases(
            *googletest,
            "MathSuite.\n  Adds\n  TypedCase  # TypeParam = int\nRenderSuite.\n  Draws\n"
        );
        context.check(cases.has_value(), "GoogleTest listing is parsed");
        if (cases) {
            context.check_equal(cases->size(), std::size_t{3}, "every GoogleTest case is normalized");
            context.check_equal(cases->front(), std::string("MathSuite.Adds"), "suite and case form a stable identifier");
        }

        const auto arguments = kaixa::test_case_arguments(*googletest, "MathSuite.Adds");
        context.check_equal(arguments.back(), std::string("--gtest_filter=MathSuite.Adds"), "case invocation uses the adapter filter");
    }

    const auto benchmark = kaixa::test_adapter("google-benchmark", kaixa::PackageTargetKind::benchmark);
    context.check(benchmark.has_value(), "Google Benchmark adapter is available");
    if (benchmark) {
        const auto cases = kaixa::parse_test_cases(*benchmark, "BM_Insert/8\nBM_Insert/64\n");
        context.check(cases && cases->size() == 2, "benchmark cases are normalized");
        const auto arguments = kaixa::test_case_arguments(*benchmark, "BM_Insert/8");
        context.check_equal(arguments.front(), std::string("--benchmark_filter=^BM_Insert/8$"), "benchmark invocation is exact");
    }
}

KAIXA_TEST(frameworks_inject_dependencies_and_generate_shared_ctest_catalogs) {
    const kaixa::testing::TempDirectory workspace("test-adapters");
    workspace.write(
        "Kaixa.toml",
        "[package-set]\n"
        "members = [\"app\", \"googletest\", \"google_benchmark\"]\n"
        "default = [\"app\"]\n"
    );
    workspace.write(
        "app/Kaixa.toml",
        "[package]\n"
        "name = \"app\"\n"
        "resolver = \"cmake\"\n"
        "tests = [\"tests\"]\n"
        "benchmarks = [\"benchmarks\"]\n"
        "\n"
        "[lib]\n"
        "sources = [\"app.cpp\"]\n"
    );
    workspace.write("app/app.cpp", "int app() { return 0; }\n");
    workspace.write(
        "app/tests/Kaixa.toml",
        "[[test]]\n"
        "name = \"app.tests\"\n"
        "sources = [\"tests.cpp\"]\n"
        "framework = \"googletest\"\n"
    );
    workspace.write("app/tests/tests.cpp", "int test_source() { return 0; }\n");
    workspace.write(
        "app/benchmarks/Kaixa.toml",
        "[[benchmark]]\n"
        "name = \"app.benchmarks\"\n"
        "sources = [\"bench.cpp\"]\n"
        "framework = \"google-benchmark\"\n"
    );
    workspace.write("app/benchmarks/bench.cpp", "int benchmark_source() { return 0; }\n");
    for (const std::string_view dependency: {"googletest", "google_benchmark"}) {
        workspace.write(
            std::string(dependency) + "/Kaixa.toml",
            "[package]\nname = \"" + std::string(dependency) + "\"\nresolver = \"cmake\"\n\n[lib]\ntype = \"interface\"\n"
        );
    }

    const auto graph = kaixa::load_workspace(workspace.path());
    context.check(graph.has_value(), "framework dependencies resolve through the package graph");
    if (!graph) {
        context.fail(kaixa::format_diagnostic(graph.error()));
        return;
    }

    const kaixa::PackageNode& root = (*graph)[graph->roots().front()];
    context.check_equal(root.target_dependencies.size(), std::size_t{2}, "each framework contributes its package dependency");

    const kaixa::ExtensionRegistry registry = kaixa::plugin::default_registry();
    const kaixa::BuildEnvironment environment{workspace.path(), workspace.path() / ".kaixa", "debug"};
    const auto plan = kaixa::plan_build(*graph, registry, environment);
    context.check(plan.has_value(), "framework workspace plans");
    if (!plan)
        return;

    const auto project = std::ranges::find_if(plan->generated_files(), [&](const kaixa::GeneratedFile& file) {
        return file.path.filename() == "CMakeLists.txt" && file.content.contains("project(app ");
    });
    context.check(project != plan->generated_files().end(), "application project is generated");
    if (project != plan->generated_files().end()) {
        context.check_contains(project->content, "gtest_main", "GoogleTest main product is linked implicitly");
        context.check_contains(project->content, "gtest_discover_tests", "GoogleTest cases are registered with CTest");
        context.check_contains(project->content, "--benchmark_list_tests=true", "benchmark cases are discovered for CTest and IDEs");
        context.check_contains(project->content, "if(NOT EXISTS", "unbuilt discovery targets do not break the selected catalog");
        context.check_contains(project->content, "kaixa.purpose:benchmark", "benchmark catalog has a normalized purpose label");
    }
}

KAIXA_TEST(runtime_files_headers_and_exports_are_generated) {
    const kaixa::testing::TempDirectory workspace("delivery-generation");
    workspace.write(
        "Kaixa.toml",
        "[package]\n"
        "name = \"delivery\"\n"
        "version = \"1.2.3\"\n"
        "resolver = \"cmake\"\n"
        "\n"
        "[lib]\n"
        "sources = [\"src/library.cpp\"]\n"
        "public-headers = [\"include/delivery/api.hpp\"]\n"
        "public-include = [\"include\"]\n"
        "runtime-files = [\"runtime/plugin.dll\"]\n"
    );
    workspace.write("src/library.cpp", "int delivery() { return 0; }\n");
    workspace.write("include/delivery/api.hpp", "#pragma once\n");
    workspace.write("runtime/plugin.dll", "runtime\n");

    const auto graph = kaixa::load_workspace(workspace.path());
    context.check(graph.has_value(), "delivery workspace loads");
    if (!graph)
        return;

    const kaixa::ExtensionRegistry registry = kaixa::plugin::default_registry();
    const kaixa::BuildEnvironment environment{workspace.path(), workspace.path() / ".kaixa", "debug"};
    const auto plan = kaixa::plan_build(*graph, registry, environment);
    context.check(plan.has_value(), "delivery workspace plans");
    if (!plan)
        return;

    const auto project = std::ranges::find_if(plan->generated_files(), [](const kaixa::GeneratedFile& file) {
        return file.path.filename() == "CMakeLists.txt";
    });
    context.check(project != plan->generated_files().end(), "delivery project is generated");
    if (project == plan->generated_files().end())
        return;

    context.check_contains(project->content, "add_custom_command(TARGET delivery POST_BUILD", "runtime materialization follows build");
    context.check_contains(project->content, "copy_if_different", "runtime files are materialized incrementally");
    context.check_contains(project->content, "DESTINATION [[include/delivery]]", "public header layout is preserved");
    context.check_contains(project->content, "install(EXPORT deliveryTargets", "consumer export is generated");
    context.check_contains(project->content, "$<INSTALL_INTERFACE:include>", "installed consumers receive the public include root");
    context.check(
        !project->content.contains(workspace.path().generic_string()),
        "source-generated projects do not contain the workspace's absolute path"
    );
}

KAIXA_TEST(install_materializes_a_runnable_tree_outside_the_build_directory) {
    const kaixa::testing::TempDirectory workspace("install-runtime");
    workspace.write(
        "Kaixa.toml",
        "[package-set]\n"
        "members = [\"app\", \"runtime_lib\"]\n"
        "default = [\"runtime_app\"]\n"
    );
    workspace.write(
        "app/Kaixa.toml",
        "[package]\n"
        "name = \"runtime_app\"\n"
        "version = \"1.0.0\"\n"
        "resolver = \"cmake\"\n"
        "\n"
        "[dependencies]\n"
        "runtime_lib = \"1.0.0\"\n"
        "\n"
        "[bin]\n"
        "sources = [\"main.cpp\"]\n"
    );
    workspace.write("app/main.cpp", "int main() { return 0; }\n");
    workspace.write(
        "runtime_lib/Kaixa.toml",
        "[package]\n"
        "name = \"runtime_lib\"\n"
        "version = \"1.0.0\"\n"
        "resolver = \"cmake\"\n"
        "\n"
        "[lib]\n"
        "sources = [\"src/library.cpp\"]\n"
        "public-headers = [\"include/runtime_lib/api.hpp\"]\n"
        "public-include = [\"include\"]\n"
        "runtime-files = [\"runtime/plugin.dat\"]\n"
    );
    workspace.write("runtime_lib/src/library.cpp", "int runtime_library() { return 0; }\n");
    workspace.write("runtime_lib/include/runtime_lib/api.hpp", "#pragma once\n");
    workspace.write("runtime_lib/runtime/plugin.dat", "runtime\n");

    const auto graph = kaixa::load_workspace(workspace.path());
    context.check(graph.has_value(), "runtime application loads");
    if (!graph)
        return;

    const kaixa::ExtensionRegistry registry = kaixa::plugin::default_registry();
    const kaixa::BuildEnvironment environment{workspace.path(), workspace.path() / ".kaixa", "debug"};
    const std::filesystem::path prefix = workspace.path() / "distribution";
    kaixa::BuildRequest request;
    request.install = true;
    request.install_prefix = prefix;
    const auto plan = kaixa::plan_build(*graph, registry, environment, request);
    context.check(plan.has_value(), "install plan is created");
    if (!plan) {
        context.fail(kaixa::format_diagnostic(plan.error()));
        return;
    }

    for (const kaixa::GeneratedFile& file: plan->generated_files()) {
        if (file.path.filename() == "CMakeLists.txt") {
            context.check(
                !file.content.contains(workspace.path().generic_string()),
                "workspace projects do not contain the workspace's absolute path"
            );
        }
    }

    const auto installed = kaixa::execute(*plan);
    context.check(installed.has_value(), "install plan executes");
    if (!installed) {
        context.fail(kaixa::format_diagnostic(installed.error()));
        return;
    }

#ifdef _WIN32
    const std::filesystem::path executable = prefix / "bin/runtime_app.exe";
#else
    const std::filesystem::path executable = prefix / "bin/runtime_app";
#endif
    context.check(std::filesystem::is_regular_file(executable), "installed executable exists outside the build tree");
    context.check(std::filesystem::is_regular_file(prefix / "bin/plugin.dat"), "runtime file is installed beside the executable");
    context.check(
        std::filesystem::is_regular_file(prefix / "include/runtime_lib/api.hpp"),
        "public dependency header is installed for consumers"
    );
    context.check(
        std::filesystem::is_regular_file(prefix / "lib/cmake/runtime_lib/runtime_libConfig.cmake"),
        "dependency package export is installed"
    );
}

KAIXA_TEST(runtime_materialization_rejects_colliding_destinations) {
    const kaixa::testing::TempDirectory collision("runtime-collision");
    collision.write(
        "Kaixa.toml",
        "[package]\nname = \"collision\"\nresolver = \"cmake\"\n\n"
        "[bin]\nsources = [\"main.cpp\"]\nruntime-files = [\"one/plugin.dat\", \"two/plugin.dat\"]\n"
    );
    collision.write("main.cpp", "int main() { return 0; }\n");
    collision.write("one/plugin.dat", "one\n");
    collision.write("two/plugin.dat", "two\n");
    const kaixa::ExtensionRegistry registry = kaixa::plugin::default_registry();
    const auto collision_graph = kaixa::load_workspace(collision.path());
    context.check(collision_graph.has_value(), "colliding runtime sources load descriptively");
    if (collision_graph) {
        const kaixa::BuildEnvironment environment{collision.path(), collision.path() / ".kaixa", "debug"};
        const auto plan = kaixa::plan_build(*collision_graph, registry, environment);
        context.check(!plan.has_value(), "runtime destination collision is rejected");
        if (!plan)
            context.check_contains(plan.error().message, "more than one source", "collision diagnostic identifies duplicate ownership");
    }
}

KAIXA_TEST(resources_are_not_part_of_the_product_model) {
    const kaixa::testing::TempDirectory workspace("resources-rejected");
    workspace.write(
        "Kaixa.toml",
        "[package]\nname = \"resources_rejected\"\nresolver = \"cmake\"\n\n"
        "[lib]\nsources = [\"library.cpp\"]\n\n"
        "[lib.resources]\ndata = \"data\"\n"
    );
    workspace.write("library.cpp", "int value() { return 0; }\n");

    const auto graph = kaixa::load_workspace(workspace.path());
    context.check(graph.has_value(), "workspace remains descriptive until product realization");
    if (!graph)
        return;

    const auto package = kaixa::realize_package(*graph, graph->roots().front());
    context.check(!package.has_value(), "resources is rejected");
    if (!package)
        context.check_contains(package.error().message, "resources", "diagnostic identifies the removed field");
}
