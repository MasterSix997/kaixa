#include <test_support.hpp>

#include <kaixa/foundation/filesystem.hpp>
#include <kaixa/foundation/process.hpp>
#include <kaixa/kaixa.hpp>
#include <kaixa/plugin/bundle.hpp>

#include <algorithm>
#include <filesystem>
#include <string>

namespace {
    void write_remote_memory_package(const kaixa::testing::TempDirectory& root) {
        root.write(
            "remote_memory/Kaixa.toml",
            "[package]\n"
            "name = \"memory_core\"\n"
            "version = \"1.0.0\"\n"
            "resolver = \"cmake\"\n"
            "\n"
            "[lib]\n"
            "sources = [\"src/memory.cpp\"]\n"
            "public-headers = [\"include/memory_core/memory.hpp\"]\n"
            "public-include = [\"include\"]\n"
        );
        root.write("remote_memory/include/memory_core/memory.hpp", "#pragma once\nint memory_value();\n");
        root.write("remote_memory/src/memory.cpp", "#include <memory_core/memory.hpp>\nint memory_value() { return 40; }\n");
    }

    void write_workspace(const kaixa::testing::TempDirectory& root) {
        root.write(
            "workspace/Kaixa.toml",
            "[package-set]\n"
            "members = [\"error\", \"threading\", \"runner\"]\n"
            "default = [\"runner\"]\n"
            "\n"
            "[package-set.policy]\n"
            "cxx = 23\n"
            "\n"
            "[[provider]]\n"
            "name = \"local\"\n"
            "driver = \"kaixa-registry\"\n"
            "default = true\n"
            "index = \"../registry/index.toml\"\n"
        );
        root.write(
            "workspace/error/Kaixa.toml",
            "[package]\n"
            "name = \"error\"\n"
            "version = \"1.0.0\"\n"
            "resolver = \"cmake\"\n"
            "tests = [\"tests\"]\n"
            "\n"
            "[public-dependencies]\n"
            "memory_core = \"1\"\n"
            "\n"
            "[lib]\n"
            "sources = [\"src/error.cpp\"]\n"
            "public-headers = [\"include/sample/error.hpp\"]\n"
            "public-include = [\"include\"]\n"
        );
        root.write("workspace/error/include/sample/error.hpp", "#pragma once\nint error_value();\n");
        root.write(
            "workspace/error/src/error.cpp",
            "#include <sample/error.hpp>\n#include <memory_core/memory.hpp>\nint error_value() { return memory_value() + 1; }\n"
        );
        root.write(
            "workspace/error/tests/Kaixa.test.toml",
            "[[test]]\n"
            "name = \"error.tests\"\n"
            "sources = [\"error_tests.cpp\"]\n"
            "discover = true\n"
            "\n"
            "[[test]]\n"
            "name = \"error.tests.noexcept\"\n"
            "sources = [\"noexcept.cpp\"]\n"
            "policy = { exceptions = false }\n"
        );
        root.write(
            "workspace/error/tests/error_tests.cpp",
            "#include <sample/error.hpp>\n"
            "#include <iostream>\n"
            "#include <string_view>\n"
            "int main(int argc, char** argv) {\n"
            "    if (argc > 1 && std::string_view(argv[1]) == \"--kaixa-test-list\") {\n"
            "        std::cout << \"Error.ReturnsValue\\n\";\n"
            "        return 0;\n"
            "    }\n"
            "    return error_value() == 41 ? 0 : 1;\n"
            "}\n"
        );
        root.write(
            "workspace/error/tests/noexcept.cpp",
            "#include <sample/error.hpp>\nint main() { return error_value() == 41 ? 0 : 1; }\n"
        );
        root.write(
            "workspace/threading/Kaixa.toml",
            "[package]\n"
            "name = \"threading\"\n"
            "version = \"1.0.0\"\n"
            "resolver = \"cmake\"\n"
            "benchmarks = [\"benchmarks\"]\n"
            "\n"
            "[public-dependencies]\n"
            "error = \"1\"\n"
            "\n"
            "[lib]\n"
            "sources = [\"src/threading.cpp\"]\n"
            "public-headers = [\"include/sample/threading.hpp\"]\n"
            "public-include = [\"include\"]\n"
        );
        root.write("workspace/threading/include/sample/threading.hpp", "#pragma once\nint threaded_value();\n");
        root.write(
            "workspace/threading/src/threading.cpp",
            "#include <sample/error.hpp>\n#include <sample/threading.hpp>\nint threaded_value() { return error_value() + 1; }\n"
        );
        root.write(
            "workspace/threading/benchmarks/Kaixa.benchmark.toml",
            "[[benchmark]]\n"
            "name = \"threading.benchmarks\"\n"
            "sources = [\"threading_benchmarks.cpp\"]\n"
        );
        root.write(
            "workspace/threading/benchmarks/threading_benchmarks.cpp",
            "#include <sample/threading.hpp>\nint main() { return threaded_value() == 42 ? 0 : 1; }\n"
        );
        root.write(
            "workspace/runner/Kaixa.toml",
            "[package]\n"
            "name = \"runner\"\n"
            "version = \"1.0.0\"\n"
            "resolver = \"cmake\"\n"
            "\n"
            "[dependencies]\n"
            "threading = \"1\"\n"
            "\n"
            "[bin]\n"
            "sources = [\"main.cpp\"]\n"
            "runtime-files = [\"runtime/data.txt\"]\n"
            "\n"
            "[workflow.conformance]\n"
            "steps = [\"generate\", \"build\", \"test\", \"bench\"]\n"
        );
        root.write("workspace/runner/main.cpp", "#include <sample/threading.hpp>\nint main() { return threaded_value() == 42 ? 0 : 1; }\n");
        root.write("workspace/runner/runtime/data.txt", "conformance data\n");
    }

    kaixa::ResolutionOptions resolution_options(
        kaixa::ExtensionRegistry& registry,
        const std::filesystem::path& cache,
        const kaixa::LockMode lock_mode
    ) {
        kaixa::ResolutionOptions options;
        options.extensions = &registry;
        options.source_cache = cache;
        options.policy_context = {"debug", kaixa::host_target_os()};
        options.lock_mode = lock_mode;
        return options;
    }
}

KAIXA_TEST(workspace_conformance_runs_a_reproducible_vertical_slice) {
    const kaixa::testing::TempDirectory root("workspace-conformance");
    write_remote_memory_package(root);
    write_workspace(root);

    const auto published = kaixa::publish_package({root.path() / "remote_memory", root.path() / "registry"});
    context.check(published.has_value(), "source dependency publishes to the local registry");
    if (!published) {
        context.fail(kaixa::format_diagnostic(published.error()));
        return;
    }

    kaixa::ExtensionRegistry registry = kaixa::plugin::default_registry();
    auto update_options = resolution_options(registry, root.path() / "cache", kaixa::LockMode::update);
    auto resolution = kaixa::resolve_workspace(root.path() / "workspace", update_options);
    context.check(resolution.has_value(), "workspace resolves and writes its lock");
    if (!resolution) {
        context.fail(kaixa::format_diagnostic(resolution.error()));
        return;
    }

    context.check_equal(resolution->model.documents.size(), std::size_t{6}, "the complete authored model is retained");
    context.check_equal(resolution->model.summary.target_documents, std::size_t{2}, "associated-target documents share the authored model");
    context.check(resolution->graph.find_by_name("memory_core").has_value(), "remote dependency enters the graph");
    const auto initial_lock = kaixa::read_file(resolution->context.lockfile);
    context.check(initial_lock.has_value(), "resolution lock is readable");
    if (!initial_lock)
        return;

    kaixa::ExtensionRegistry frozen_registry = kaixa::plugin::default_registry();
    auto frozen_options = resolution_options(frozen_registry, root.path() / "cache", kaixa::LockMode::frozen);
    auto frozen = kaixa::resolve_workspace(root.path() / "workspace", frozen_options);
    context.check(frozen.has_value(), "the same graph resolves offline from its lock and cache");
    if (!frozen) {
        context.fail(kaixa::format_diagnostic(frozen.error()));
        return;
    }
    const auto frozen_lock = kaixa::read_file(frozen->context.lockfile);
    context.check(frozen_lock && *frozen_lock == *initial_lock, "frozen resolution leaves the lock byte-identical");

    const auto workflow = kaixa::prepare_workflow(frozen->graph, "conformance");
    context.check(workflow.has_value(), "workflow resolves through the library API");
    if (workflow)
        context.check_equal(workflow->steps.size(), std::size_t{4}, "workflow retains every conformance stage");

    const kaixa::BuildEnvironment environment{root.path() / "workspace", root.path() / "state", "debug"};
    const std::filesystem::path prefix = root.path() / "install";
    kaixa::BuildRequest install_request;
    install_request.install = true;
    install_request.install_prefix = prefix;
    const auto install_plan = kaixa::plan_build(frozen->graph, frozen_registry, environment, install_request);
    context.check(install_plan.has_value(), "workspace produces an install plan");
    if (!install_plan) {
        context.fail(kaixa::format_diagnostic(install_plan.error()));
        return;
    }

    for (const kaixa::GeneratedFile& generated: install_plan->generated_files()) {
        if (generated.path.filename() == "CMakeLists.txt") {
            context.check(
                !generated.content.contains((root.path() / "workspace").generic_string()),
                "generated projects do not embed the workspace path"
            );
        }
    }
    const auto installed = kaixa::execute(*install_plan);
    context.check(installed.has_value(), "workspace builds and installs");
    if (!installed) {
        context.fail(kaixa::format_diagnostic(installed.error()));
        return;
    }

#ifdef _WIN32
    const std::filesystem::path executable = prefix / "bin/runner.exe";
#else
    const std::filesystem::path executable = prefix / "bin/runner";
#endif
    context.check(std::filesystem::is_regular_file(executable), "installed executable exists");
    context.check(std::filesystem::is_regular_file(prefix / "bin/data.txt"), "runtime file follows the installed executable");
    const auto ran = kaixa::run_process({{executable.string()}, prefix, {}, kaixa::ProcessOutputMode::capture});
    context.check(ran.has_value() && ran->succeeded(), "installed executable runs outside the build tree");

    kaixa::TestRequest tests;
    const auto test_plan = kaixa::plan_tests(frozen->graph, frozen_registry, environment, tests);
    context.check(test_plan.has_value(), "normalized test plan");
    if (test_plan) {
        const auto tested = kaixa::test(*test_plan);
        context.check(tested.has_value(), "normalized tests execute");
        if (!tested)
            context.fail(kaixa::format_diagnostic(tested.error()));
    }

    kaixa::TestRequest benchmarks;
    benchmarks.purpose = kaixa::ProductPurpose::benchmark;
    const auto benchmark_plan = kaixa::plan_tests(frozen->graph, frozen_registry, environment, benchmarks);
    context.check(benchmark_plan.has_value(), "normalized benchmark plan");
    if (benchmark_plan) {
        const auto benchmarked = kaixa::test(*benchmark_plan);
        context.check(benchmarked.has_value(), "normalized benchmarks execute");
        if (!benchmarked)
            context.fail(kaixa::format_diagnostic(benchmarked.error()));
    }
}
