#include <test_support.hpp>

#include <kaixa/kaixa.hpp>
#include <kaixa/plugin/bundle.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>

namespace {
    const std::filesystem::path source_directory = KAIXA_SOURCE_DIR;
}

KAIXA_TEST(hello_cmake_example_loads_and_plans) {
    const std::filesystem::path example = source_directory / "examples" / "hello_cmake";
    const auto graph = kaixa::load_workspace(example);
    context.check(graph.has_value(), "example workspace loads");
    if (!graph) {
        context.fail(kaixa::format_diagnostic(graph.error()));
        return;
    }

    context.check_equal(graph->size(), std::size_t{1}, "one package");
    context.check_equal((*graph)[graph->root()].name, std::string("hello_cmake"), "package name");

    const kaixa::ResolverRegistry registry = kaixa::plugin::default_registry();
    const kaixa::BuildEnvironment environment{example, example / ".test-output", "debug"};
    const auto plan = kaixa::plan_build(*graph, registry, environment);
    context.check(plan.has_value(), "example plans");
    if (plan) {
        context.check_equal(plan->actions().size(), std::size_t{2}, "configure and build");
        context.check_equal(
            plan->actions().front().description,
            std::string("configure hello_cmake"),
            "configure action"
        );
    }
}

KAIXA_TEST(local_workspace_example_models_managed_and_opaque_packages) {
    const std::filesystem::path example = source_directory / "examples" / "local_workspace";
    const auto graph = kaixa::load_workspace(example);
    context.check(graph.has_value(), "workspace example loads");
    if (!graph) {
        context.fail(kaixa::format_diagnostic(graph.error()));
        return;
    }

    context.check_equal(graph->size(), std::size_t{3}, "three packages");
    const auto math = graph->find_by_name("demo_math");
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
    const kaixa::BuildEnvironment environment{example, example / ".test-output", "debug"};
    const auto plan = kaixa::plan_build(*graph, registry, environment);
    context.check(plan.has_value(), "workspace example plans");
    if (plan)
        context.check_equal(plan->actions().size(), std::size_t{4}, "opaque package is skipped");
}
