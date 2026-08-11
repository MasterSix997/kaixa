#include <test_support.hpp>

#include <kaixa/kaixa.hpp>
#include <kaixa/plugin/bundle.hpp>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

using kaixa::testing::TempDirectory;

KAIXA_TEST(toml_errors_keep_the_source_position) {
    const auto manifest = kaixa::parse_manifest_string(
        "[package\nname = \"broken\"\n",
        "broken.toml"
    );

    context.check(!manifest.has_value(), "invalid TOML is rejected");
    if (!manifest) {
        const std::string diagnostic = kaixa::format_diagnostic(manifest.error());
        context.check_contains(diagnostic, "broken.toml:1", "source and line");
    }
}

KAIXA_TEST(manifest_requires_a_resolver_and_local_dependency_table) {
    const auto missing_resolver = kaixa::parse_manifest_string(
        "[package]\nname = \"app\"\n",
        "missing-resolver.toml"
    );
    context.check(!missing_resolver.has_value(), "resolver is required");
    if (!missing_resolver) {
        context.check_contains(
            kaixa::format_diagnostic(missing_resolver.error()),
            "package.resolver",
            "required key path"
        );
    }

    const auto shorthand = kaixa::parse_manifest_string(
        "[package]\nname = \"app\"\nresolver = \"cmake\"\n"
        "\n[dependencies]\nmath = \"../math\"\n",
        "dependency.toml"
    );
    context.check(!shorthand.has_value(), "dependency shorthand is not accepted yet");
    if (!shorthand) {
        context.check_contains(
            kaixa::format_diagnostic(shorthand.error()),
            "local dependencies",
            "diagnostic explains the local dependency syntax"
        );
    }
}

KAIXA_TEST(find_manifest_walks_from_a_nested_file) {
    const TempDirectory root("manifest-search");
    root.write_manifest("Kaixa.toml", kaixa::Manifest{"root", "cmake"});
    root.write("nested/deeper/file.txt", "content\n");

    const auto manifest = kaixa::find_manifest(root.path() / "nested" / "deeper" / "file.txt");
    context.check(manifest.has_value(), "manifest is found from a nested file");
    if (manifest) {
        context.check_equal(
            manifest->lexically_normal().generic_string(),
            (root.path() / "Kaixa.toml").lexically_normal().generic_string(),
            "nearest manifest"
        );
    }
}

KAIXA_TEST(workspace_rejects_a_dependency_with_the_wrong_package_name) {
    const TempDirectory root("name-mismatch");
    kaixa::Manifest app{"app", "cmake"};
    app.dependencies.emplace_back("math", "math");
    root.write_manifest("Kaixa.toml", app);
    root.write_manifest("math/Kaixa.toml", kaixa::Manifest{"not_math", "cmake"});

    const auto graph = kaixa::load_workspace(root.path());
    context.check(!graph.has_value(), "mismatched package name is rejected");
    if (!graph) {
        context.check_contains(
            kaixa::format_diagnostic(graph.error()),
            "points to package `not_math`",
            "diagnostic names the actual package"
        );
    }
}

KAIXA_TEST(workspace_rejects_a_missing_local_dependency) {
    const TempDirectory root("missing-dependency");
    kaixa::Manifest app{"app", "cmake"};
    app.dependencies.emplace_back("missing", "does-not-exist");
    root.write_manifest("Kaixa.toml", app);

    const auto graph = kaixa::load_workspace(root.path());
    context.check(!graph.has_value(), "missing dependency is rejected");
    if (!graph) {
        context.check_contains(
            kaixa::format_diagnostic(graph.error()),
            "directory does not exist",
            "diagnostic explains the failure"
        );
    }
}

KAIXA_TEST(cmake_options_select_the_source_and_build_arguments_select_the_generator) {
    const TempDirectory root("cmake-options");
    kaixa::Manifest manifest{"configured", "cmake"};
    manifest.resolver_options = kaixa::Value::table({{"source", "project"}});
    root.write_manifest("Kaixa.toml", manifest);
    root.write("project/CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)\n");

    const auto graph = kaixa::load_workspace(root.path());
    if (!graph) {
        context.fail(kaixa::format_diagnostic(graph.error()));
        return;
    }

    const kaixa::ResolverRegistry registry = kaixa::plugin::default_registry();
    kaixa::EffectiveBuildConfiguration configuration;
    configuration.profile = "release";
    configuration.resolvers.push_back({"cmake", std::nullopt, {"-G", "Ninja"}});
    const kaixa::BuildEnvironment environment{
        root.path(),
        root.path() / "out",
        std::move(configuration)
    };
    const auto plan = kaixa::plan_build(*graph, registry, environment);
    context.check(plan.has_value(), "configured CMake package plans");
    if (!plan || plan->actions().empty())
        return;

    const std::span<const kaixa::Action> actions = plan->actions();
    const std::vector<std::string>& command = actions.front().argv;
    context.check(
        std::ranges::find(command, "Ninja") != command.end(),
        "requested generator is forwarded"
    );
    context.check(
        std::ranges::find(command, "-DCMAKE_BUILD_TYPE=Release") != command.end(),
        "single-config generator receives the profile"
    );
    context.check(
        std::ranges::find(command, (root.path() / "project").string()) != command.end(),
        "configured source directory is forwarded"
    );
}

KAIXA_TEST(cmake_rejects_an_unknown_dependency_mode) {
    const TempDirectory root("cmake-dependency-mode");
    kaixa::Manifest app{"app", "cmake"};
    app.dependencies.emplace_back("math", "math");
    app.resolver_options = kaixa::Value::table({
        {"dependencies", kaixa::Value::table({{"math", "magic"}})}
    });
    root.write_manifest("Kaixa.toml", app);
    root.write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)\n");
    root.write_manifest("math/Kaixa.toml", kaixa::Manifest{"math", "cmake"});
    root.write("math/CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)\n");

    const auto graph = kaixa::load_workspace(root.path());
    if (!graph) {
        context.fail(kaixa::format_diagnostic(graph.error()));
        return;
    }

    const kaixa::ResolverRegistry registry = kaixa::plugin::default_registry();
    const kaixa::BuildEnvironment environment{root.path(), root.path() / "out", "debug"};
    const auto plan = kaixa::plan_build(*graph, registry, environment);
    context.check(!plan.has_value(), "unknown mode is rejected");
    if (!plan) {
        context.check_contains(
            kaixa::format_diagnostic(plan.error()),
            "expected `add-subdirectory` or `find-package`",
            "diagnostic lists supported modes"
        );
    }
}

KAIXA_TEST(cmake_rejects_singular_and_plural_target_forms_together) {
    const TempDirectory root("cmake-target-forms");
    root.write(
        "Kaixa.toml",
        "[package]\nname = \"mixed\"\nresolver = \"cmake\"\n"
        "\n[cmake.target]\ntype = \"executable\"\nsources = [\"main.cpp\"]\n"
        "\n[cmake.targets.other]\ntype = \"executable\"\nsources = [\"main.cpp\"]\n"
    );
    root.write("main.cpp", "int main() { return 0; }\n");

    const auto graph = kaixa::load_workspace(root.path());
    if (!graph) {
        context.fail(kaixa::format_diagnostic(graph.error()));
        return;
    }

    const kaixa::ResolverRegistry registry = kaixa::plugin::default_registry();
    const kaixa::BuildEnvironment environment{root.path(), root.path() / "out", "debug"};
    const auto plan = kaixa::plan_build(*graph, registry, environment);
    context.check(!plan.has_value(), "mixed target forms are rejected");
    if (plan)
        return;

    context.check_contains(
        kaixa::format_diagnostic(plan.error()),
        "cannot be used together",
        "diagnostic explains target form conflict"
    );
}

KAIXA_TEST(cmake_forwards_compilers_toolchain_and_arguments) {
    const TempDirectory root("cmake-configure-options");
    root.write_manifest("Kaixa.toml", kaixa::Manifest{"configured", "cmake"});
    root.write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)\n");
    root.write("toolchain.cmake", "# test fixture\n");

    const auto graph = kaixa::load_workspace(root.path());
    if (!graph) {
        context.fail(kaixa::format_diagnostic(graph.error()));
        return;
    }

    const kaixa::ResolverRegistry registry = kaixa::plugin::default_registry();
    kaixa::EffectiveBuildConfiguration configuration;
    configuration.profile = "debug";
    configuration.resolvers.push_back({
        "cmake",
        std::nullopt,
        {
            "-G", "Ninja",
            "-DCMAKE_C_COMPILER=clang",
            "-DCMAKE_CXX_COMPILER=clang++",
            "-DCMAKE_TOOLCHAIN_FILE=" + (root.path() / "toolchain.cmake").string(),
            "-DBUILD_TESTING=OFF",
            "--fresh"
        }
    });
    const kaixa::BuildEnvironment environment{
        root.path(),
        root.path() / "out",
        std::move(configuration)
    };
    const auto plan = kaixa::plan_build(*graph, registry, environment);
    context.check(plan.has_value(), "CMake configure options plan");
    if (!plan || plan->actions().empty())
        return;

    const std::vector<std::string> command = plan->actions().front().argv;
    for (const std::string& expected: {
             std::string("Ninja"),
             std::string("-DCMAKE_C_COMPILER=clang"),
             std::string("-DCMAKE_CXX_COMPILER=clang++"),
             std::string("-DCMAKE_TOOLCHAIN_FILE=") + (root.path() / "toolchain.cmake").string(),
             std::string("-DBUILD_TESTING=OFF"),
             std::string("--fresh")
         }) {
        context.check(
            std::ranges::find(command, expected) != command.end(),
            "configure command contains " + expected
        );
    }
}

KAIXA_TEST(cmake_rejects_machine_configuration_in_the_package) {
    const TempDirectory root("cmake-package-machine-config");
    kaixa::Manifest manifest{"portable", "cmake"};
    manifest.resolver_options = kaixa::Value::table({{"generator", "Ninja"}});
    root.write_manifest("Kaixa.toml", manifest);
    root.write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)\n");

    const auto graph = kaixa::load_workspace(root.path());
    if (!graph) {
        context.fail(kaixa::format_diagnostic(graph.error()));
        return;
    }

    const kaixa::ResolverRegistry registry = kaixa::plugin::default_registry();
    const kaixa::BuildEnvironment environment{root.path(), root.path() / "out", "debug"};
    const auto plan = kaixa::plan_build(*graph, registry, environment);
    context.check(!plan.has_value(), "package cannot select a local generator");
    if (!plan) {
        context.check_contains(
            kaixa::format_diagnostic(plan.error()),
            "unknown key `cmake.generator`",
            "diagnostic rejects machine configuration"
        );
    }
}
