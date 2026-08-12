#include <test_support.hpp>

#include <kaixa/kaixa.hpp>
#include <kaixa/plugin/bundle.hpp>

#include <cstddef>
#include <string>
#include <utility>

using kaixa::testing::TempDirectory;

KAIXA_TEST(manifest_reads_the_current_schema) {
    const auto manifest = kaixa::parse_manifest_string(
        "[package]\n"
        "name = \"app\"\n"
        "version = \"0.1.0\"\n"
        "resolver = \"cmake\"\n"
        "\n"
        "[dependencies]\n"
        "math = { path = \"../math\" }\n",
        "manifest-test"
    );

    context.check(manifest.has_value(), "valid manifest parses");
    if (!manifest) {
        context.fail(kaixa::format_diagnostic(manifest.error()));
        return;
    }

    context.check_equal(manifest->name, std::string("app"), "package name");
    context.check_equal(manifest->resolver, std::string("cmake"), "resolver");
    context.check_equal(manifest->dependencies.size(), std::size_t{1}, "dependency count");
    if (!manifest->dependencies.empty()) {
        context.check_equal(
            manifest->dependencies.front().path.generic_string(),
            std::string("../math"),
            "dependency path"
        );
    }
}

KAIXA_TEST(manifest_rejects_unknown_keys) {
    const auto manifest = kaixa::parse_manifest_string(
        "[package]\nname = \"app\"\nresolver = \"cmake\"\ntypo = true\n",
        "invalid-manifest"
    );

    context.check(!manifest.has_value(), "unknown package key is rejected");
    if (!manifest) {
        context.check_contains(
            kaixa::format_diagnostic(manifest.error()),
            "package.typo",
            "diagnostic names the unknown key"
        );
    }
}

KAIXA_TEST(programmatic_manifest_formats_and_round_trips) {
    kaixa::Manifest authored{"app", "cmake"};
    authored.version = kaixa::Version{"0.1.0"};
    authored.dependencies.emplace_back("math", "../math");
    authored.configurations.defaults.push_back("dev");

    kaixa::ConfigurationDefinition dev;
    dev.name = "dev";
    dev.profile = "debug";
    dev.resolvers.push_back({
        "cmake",
        kaixa::Value::table({
            {"generator", "Ninja"},
            {"arguments", kaixa::Value::array({"-DDEV=ON"})}
        })
    });
    authored.configurations.definitions.push_back(std::move(dev));
    authored.resolver_options = kaixa::Value::table({
        {"source", "project"},
        {"cxx-standard", 23},
        {"target", kaixa::Value::array({kaixa::Value::table({
            {"name", "app"},
            {"type", "executable"},
            {"sources", kaixa::Value::array({"src/main.cpp"})}
        })})}
    });

    const auto text = kaixa::format_manifest(authored);
    context.check(text.has_value(), "programmatic manifest formats");
    if (!text) {
        context.fail(kaixa::format_diagnostic(text.error()));
        return;
    }
    context.check_contains(*text, "[[config]]", "canonical configuration syntax");
    context.check_contains(*text, "[[cmake.target]]", "canonical target syntax");

    const auto parsed = kaixa::parse_manifest_string(*text, "generated-manifest");
    context.check(parsed.has_value(), "formatted manifest parses");
    if (!parsed) {
        context.fail(kaixa::format_diagnostic(parsed.error()));
        return;
    }
    context.check_equal(parsed->name, authored.name, "package name round-trips");
    context.check_equal(
        parsed->dependencies.front().name,
        std::string("math"),
        "dependency round-trips"
    );
    context.check_equal(
        parsed->configurations.defaults.front(),
        std::string("dev"),
        "configuration round-trips"
    );
    context.check(parsed->resolver_options.has_value(), "resolver options round-trip");
}

KAIXA_TEST(programmatic_manifest_rejects_invalid_data) {
    kaixa::Manifest manifest{"app", "cmake"};
    manifest.dependencies.emplace_back("math", "first");
    manifest.dependencies.emplace_back("math", "second");

    const auto text = kaixa::format_manifest(manifest);
    context.check(!text.has_value(), "invalid programmatic manifest is rejected");
    if (!text) {
        context.check_contains(
            kaixa::format_diagnostic(text.error()),
            "duplicate dependency `math`",
            "diagnostic explains invalid model"
        );
    }
}

KAIXA_TEST(workspace_orders_local_dependencies_and_plans_cmake) {
    const TempDirectory root("workspace");
    kaixa::Manifest app{"app", "cmake"};
    app.dependencies.emplace_back("math", "math");
    root.write_manifest("Kaixa.toml", app);
    root.write_manifest("math/Kaixa.toml", kaixa::Manifest{"math", "cmake"});
    root.write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)\n");
    root.write("math/CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)\n");

    const auto graph = kaixa::load_workspace(root.path());
    context.check(graph.has_value(), "workspace loads");
    if (!graph) {
        context.fail(kaixa::format_diagnostic(graph.error()));
        return;
    }

    context.check_equal(graph->size(), std::size_t{2}, "package count");
    const auto order = graph->build_order();
    context.check(order.has_value(), "workspace has a build order");
    if (order && order->size() == 2) {
        context.check_equal((*graph)[(*order)[0]].name, std::string("math"), "dependency first");
        context.check_equal((*graph)[(*order)[1]].name, std::string("app"), "root last");
    }

    const kaixa::ResolverRegistry registry = kaixa::plugin::default_registry();
    const kaixa::BuildEnvironment environment{
        root.path(),
        root.path() / "out",
        "debug"
    };
    const auto plan = kaixa::plan_build(*graph, registry, environment);
    context.check(plan.has_value(), "CMake resolver creates a plan");
    if (!plan)
        return;

    context.check_equal(plan->actions().size(), std::size_t{2}, "one composed configure and build");
    context.check_equal(plan->generated_files().size(), std::size_t{2}, "integration and File API query");
    if (plan->actions().size() == 2) {
        context.check(
            plan->actions()[0].stage == kaixa::ActionStage::synchronize,
            "configure synchronizes"
        );
        context.check(plan->actions()[1].stage == kaixa::ActionStage::build, "build stays explicit");
    }
}

KAIXA_TEST(graph_rejects_dependency_cycles) {
    kaixa::Graph graph;
    const kaixa::PackageId first = graph.add({
        {}, "first", {}, kaixa::PackageKind::managed, "cmake", std::nullopt, {}
    });
    const kaixa::PackageId second = graph.add({
        {}, "second", {}, kaixa::PackageKind::managed, "cmake", std::nullopt, {}
    });
    graph[first].dependencies.push_back(second);
    graph[second].dependencies.push_back(first);

    const auto order = graph.build_order();
    context.check(!order.has_value(), "cycle is rejected");
    if (!order)
        context.check_contains(order.error().message, "first -> second -> first", "cycle path");
}
