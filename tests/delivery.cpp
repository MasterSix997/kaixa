#include <test_support.hpp>

#include <kaixa/kaixa.hpp>
#include <kaixa/plugin/bundle.hpp>

#include <algorithm>
#include <filesystem>
#include <string>

KAIXA_TEST(test_adapters_normalize_framework_cases_and_invocations) {
    kaixa::ExtensionRegistry registry = kaixa::plugin::default_registry();
    const auto googletest = kaixa::test_adapter(registry, "googletest", kaixa::PackageTargetKind::test);
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

    const auto benchmark = kaixa::test_adapter(registry, "google-benchmark", kaixa::PackageTargetKind::benchmark);
    context.check(benchmark.has_value(), "Google Benchmark adapter is available");
    if (benchmark) {
        const auto cases = kaixa::parse_test_cases(*benchmark, "BM_Insert/8\nBM_Insert/64\n");
        context.check(cases && cases->size() == 2, "benchmark cases are normalized");
        const auto arguments = kaixa::test_case_arguments(*benchmark, "BM_Insert/8");
        context.check_equal(arguments.front(), std::string("--benchmark_filter=^BM_Insert/8$"), "benchmark invocation is exact");
    }

    registry.add(
        kaixa::TestAdapterInfo{"project-tests",
            kaixa::TestAdapterPurpose::test,
            {},
            {},
            {"--list"},
            "--case=",
            {},
            kaixa::TestCaseListingFormat::lines}
    );
    const auto custom = kaixa::test_adapter(registry, "project-tests", kaixa::PackageTargetKind::test);
    context.check(custom.has_value(), "extensions can register a project-specific test adapter");
    if (custom) {
        const auto cases = kaixa::parse_test_cases(*custom, "alpha\nbeta\n");
        context.check(cases && cases->size() == 2, "custom adapter uses its declared listing format");
        const auto arguments = kaixa::test_case_arguments(*custom, "alpha");
        context.check_equal(arguments.front(), std::string("--case=alpha"), "custom adapter owns case invocation");
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
        "include = [\"support\"]\n"
        "system-include = [\"vendor/include\"]\n"
        "defines = { CASE_ROOT = { path = \"fixtures\" } }\n"
        "system-libraries = [\"threads\"]\n"
        "install = true\n"
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
        context.check_contains(project->content, "googletest", "GoogleTest default product is linked implicitly");
        context.check_contains(project->content, "gtest_discover_tests", "GoogleTest cases are registered with CTest");
        context.check_contains(project->content, "--benchmark_list_tests=true", "benchmark cases are discovered for CTest and IDEs");
        context.check_contains(project->content, "if(NOT EXISTS", "unbuilt discovery targets do not break the selected catalog");
        context.check_contains(project->content, "kaixa.purpose:benchmark", "benchmark catalog has a normalized purpose label");
        context.check_contains(project->content, "tests/support", "target include is relative to its manifest");
        context.check_contains(project->content, "tests/vendor/include", "target system include is relative to its manifest");
        context.check_contains(project->content, "CASE_ROOT=", "target definition is generated");
        context.check_contains(project->content, "app/tests/fixtures", "definition path is relative to its manifest");
        context.check_contains(project->content, "threads", "target system library is linked");
        context.check_contains(project->content, "install(TARGETS app.tests", "installable associated target is exported");
    }
}

KAIXA_TEST(prebuilt_descriptors_reach_cmake_consumers) {
    const kaixa::testing::TempDirectory workspace("prebuilt-consumer");
    workspace.write(
        "Kaixa.toml",
        "[package]\n"
        "name = \"application\"\n"
        "resolver = \"cmake\"\n"
        "\n"
        "[dependencies]\n"
        "toolkit = \"1\"\n"
        "\n"
        "[bin]\n"
        "sources = [\"source.cpp\"]\n"
        "\n"
        "[providers.binary]\n"
        "driver = \"package-map\"\n"
        "default = true\n"
        "\n"
        "[[providers.binary.package]]\n"
        "name = \"toolkit\"\n"
        "version = \"1.0.0\"\n"
        "artifact = { driver = \"path\", path = \"toolkit\" }\n"
        "include = [\"include\"]\n"
        "system-include = [\"system\"]\n"
        "libraries = [\"lib/toolkit.lib\"]\n"
        "system-libraries = [\"user32\"]\n"
        "runtime-files = [\"bin/*.dll\"]\n"
    );
    workspace.write("source.cpp", "int main() { return 0; }\n");
    workspace.write("toolkit/include/toolkit.hpp", "#pragma once\n");
    workspace.write("toolkit/system/toolkit_detail.hpp", "#pragma once\n");
    workspace.write("toolkit/lib/toolkit.lib", "library\n");
    workspace.write("toolkit/bin/toolkit.dll", "runtime\n");

    kaixa::ExtensionRegistry registry = kaixa::plugin::default_registry();
    const auto resolution = kaixa::resolve_workspace(
        workspace.path(),
        kaixa::ResolutionOptions{{}, &registry, workspace.path() / ".cache"}
    );
    context.check(resolution.has_value(), "prebuilt descriptor resolves");
    if (!resolution) {
        context.fail(kaixa::format_diagnostic(resolution.error()));
        return;
    }

    const auto toolkit = resolution->graph.find_by_name("toolkit");
    context.check(toolkit.has_value(), "prebuilt package enters the graph");
    if (toolkit) {
        context.check_equal(resolution->graph[*toolkit].directory, workspace.path() / "toolkit", "artifact is materialized");
    }

    const kaixa::BuildEnvironment environment{workspace.path(), workspace.path() / ".kaixa", "debug"};
    const auto plan = kaixa::plan_build(resolution->graph, registry, environment);
    context.check(plan.has_value(), "prebuilt consumer plans");
    if (!plan)
        return;

    const auto project = std::ranges::find_if(plan->generated_files(), [](const kaixa::GeneratedFile& file) {
        return file.path.filename() == "CMakeLists.txt";
    });
    context.check(project != plan->generated_files().end(), "consumer project is generated");
    if (project == plan->generated_files().end())
        return;

    context.check_contains(project->content, "toolkit/include", "prebuilt public include reaches the consumer");
    context.check_contains(project->content, "toolkit/system", "prebuilt system include reaches the consumer");
    context.check_contains(project->content, "toolkit/lib/toolkit.lib", "prebuilt library reaches the linker");
    context.check_contains(project->content, "user32", "prebuilt system library reaches the linker");
    context.check_contains(project->content, "toolkit.dll", "prebuilt runtime file is staged");
}

KAIXA_TEST(provider_source_recipes_adopt_external_cmake_products) {
    const kaixa::testing::TempDirectory workspace("adopted-provider-source");
    workspace.write(
        "Kaixa.toml",
        "imports = [\"config/providers.toml\"]\n"
        "\n"
        "[package]\n"
        "name = \"application\"\n"
        "resolver = \"cmake\"\n"
        "\n"
        "[dependencies]\n"
        "component = \"1\"\n"
        "\n"
        "[bin]\n"
        "sources = [\"main.cpp\"]\n"
    );
    workspace.write(
        "config/providers.toml",
        "[providers.source]\n"
        "driver = \"package-map\"\n"
        "default = true\n"
        "\n"
        "[[providers.source.package]]\n"
        "name = \"component\"\n"
        "version = \"1.0.0\"\n"
        "source = { driver = \"path\", path = \"../vendor\" }\n"
        "consumer = { resolver = \"cmake\", mode = \"add-subdirectory\", path = \"Build\", options = { COMPONENT_VALUE = 42 } }\n"
        "products = { default = \"UpstreamComponent\" }\n"
    );
    workspace.write("main.cpp", "int component_value();\nint main() { return component_value() == 42 ? 0 : 1; }\n");
    workspace.write("vendor/component.cpp", "int component_value() { return COMPONENT_VALUE; }\n");
    workspace.write(
        "vendor/Build/CMakeLists.txt",
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(UpstreamComponent LANGUAGES CXX)\n"
        "add_library(UpstreamComponent STATIC ../component.cpp)\n"
        "target_compile_definitions(UpstreamComponent PRIVATE COMPONENT_VALUE=${COMPONENT_VALUE})\n"
    );

    kaixa::ExtensionRegistry registry = kaixa::plugin::default_registry();
    const auto resolution = kaixa::resolve_workspace(workspace.path(), kaixa::ResolutionOptions{{}, &registry});
    context.check(resolution.has_value(), "provider source recipe resolves");
    if (!resolution) {
        context.fail(kaixa::format_diagnostic(resolution.error()));
        return;
    }

    const auto component = resolution->graph.find_by_name("component");
    context.check(component.has_value(), "adopted source package enters the graph");
    if (!component)
        return;

    const kaixa::PackageNode& package = resolution->graph[*component];
    context.check(package.kind == kaixa::PackageKind::managed, "adopted source participates in source builds");
    context.check_equal(package.directory, workspace.path() / "vendor/Build", "consumer path selects the external project");

    const kaixa::BuildEnvironment environment{workspace.path(), workspace.path() / ".kaixa", "debug"};
    const auto plan = kaixa::plan_build(resolution->graph, registry, environment);
    context.check(plan.has_value(), "adopted source build plans");
    if (!plan) {
        context.fail(kaixa::format_diagnostic(plan.error()));
        return;
    }

    const auto root_project = std::ranges::find_if(plan->generated_files(), [&](const kaixa::GeneratedFile& file) {
        return file.path == workspace.path() / "CMakeLists.txt";
    });
    context.check(root_project != plan->generated_files().end(), "consumer project is generated");
    if (root_project != plan->generated_files().end())
        context.check_contains(root_project->content, "UpstreamComponent", "logical package links its declared default product");

    const auto integration = std::ranges::find_if(plan->generated_files(), [](const kaixa::GeneratedFile& file) {
        return file.path.filename() == "dependencies.cmake";
    });
    context.check(integration != plan->generated_files().end(), "source integration is generated");
    if (integration != plan->generated_files().end()) {
        context.check_contains(integration->content, "set(COMPONENT_VALUE 42)", "consumer option is scoped before adoption");
        context.check_contains(integration->content, "vendor/Build", "consumer subdirectory is adopted");
    }

    const auto built = kaixa::execute(*plan);
    context.check(built.has_value(), "adopted source compiles with its consumer");
    if (!built)
        context.fail(kaixa::format_diagnostic(built.error()));
}

KAIXA_TEST(shared_provider_source_recipe_is_resolved_once) {
    const kaixa::testing::TempDirectory workspace("shared-adopted-provider-source");
    workspace.write(
        "Kaixa.toml",
        "[package-set]\n"
        "name = \"workspace\"\n"
        "members = [\"first\", \"second\"]\n"
        "default = [\"first\", \"second\"]\n"
        "\n"
        "[providers.source]\n"
        "driver = \"package-map\"\n"
        "default = true\n"
        "\n"
        "[[providers.source.package]]\n"
        "name = \"component\"\n"
        "version = \"1.0.0\"\n"
        "source = { driver = \"path\", path = \"vendor\" }\n"
        "consumer = { resolver = \"cmake\", mode = \"add-subdirectory\" }\n"
        "products = { default = \"UpstreamComponent\" }\n"
    );
    for (const std::string_view package: {"first", "second"}) {
        workspace.write(
            std::string(package) + "/Kaixa.toml",
            "[package]\n"
            "name = \""
                + std::string(package)
                + "\"\n"
                  "version = \"1.0.0\"\n"
                  "resolver = \"cmake\"\n"
                  "\n"
                  "[dependencies]\n"
                  "component = \"1\"\n"
        );
    }
    workspace.write("vendor/CMakeLists.txt", "add_library(UpstreamComponent INTERFACE)\n");

    kaixa::ExtensionRegistry registry = kaixa::plugin::default_registry();
    const auto resolution = kaixa::resolve_workspace(workspace.path(), kaixa::ResolutionOptions{{}, &registry});
    context.check(resolution.has_value(), "shared provider source recipe resolves");
    if (!resolution) {
        context.fail(kaixa::format_diagnostic(resolution.error()));
        return;
    }

    context.check_equal(resolution->graph.size(), std::size_t{3}, "shared adopted source is resolved once");
}

KAIXA_TEST(provider_source_recipe_requires_a_consumer_resolver) {
    const kaixa::testing::TempDirectory workspace("provider-source-without-resolver");
    workspace.write(
        "Kaixa.toml",
        "[package]\n"
        "name = \"application\"\n"
        "resolver = \"cmake\"\n"
        "\n"
        "[dependencies]\n"
        "component = \"1\"\n"
        "\n"
        "[providers.source]\n"
        "driver = \"package-map\"\n"
        "default = true\n"
        "\n"
        "[[providers.source.package]]\n"
        "name = \"component\"\n"
        "version = \"1.0.0\"\n"
        "source = { driver = \"path\", path = \"vendor\" }\n"
        "consumer = { mode = \"add-subdirectory\" }\n"
    );

    kaixa::ExtensionRegistry registry = kaixa::plugin::default_registry();
    const auto resolution = kaixa::resolve_workspace(workspace.path(), kaixa::ResolutionOptions{{}, &registry});
    context.check(!resolution.has_value(), "consumer without resolver is rejected");
    if (!resolution) {
        context.check_contains(
            kaixa::format_diagnostic(resolution.error()),
            "[providers.source.package.0.consumer.resolver]: missing required key",
            "missing resolver diagnostic is precise"
        );
    }
}

KAIXA_TEST(provider_source_recipe_rejects_an_unsupported_consumer_mode) {
    const kaixa::testing::TempDirectory workspace("provider-source-consumer-mode");
    workspace.write(
        "Kaixa.toml",
        "[package]\n"
        "name = \"application\"\n"
        "resolver = \"cmake\"\n"
        "\n"
        "[dependencies]\n"
        "component = \"1\"\n"
        "\n"
        "[providers.source]\n"
        "driver = \"package-map\"\n"
        "default = true\n"
        "\n"
        "[[providers.source.package]]\n"
        "name = \"component\"\n"
        "version = \"1.0.0\"\n"
        "source = { driver = \"path\", path = \"vendor\" }\n"
        "consumer = { resolver = \"cmake\", mode = \"find-package\" }\n"
    );
    workspace.write("vendor/CMakeLists.txt", "add_library(UpstreamComponent INTERFACE)\n");

    kaixa::ExtensionRegistry registry = kaixa::plugin::default_registry();
    const auto resolution = kaixa::resolve_workspace(workspace.path(), kaixa::ResolutionOptions{{}, &registry});
    context.check(!resolution.has_value(), "unsupported consumer mode is rejected");
    if (!resolution) {
        context.check_contains(
            kaixa::format_diagnostic(resolution.error()),
            "requires consumer mode `add-subdirectory`",
            "unsupported mode diagnostic is precise"
        );
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
