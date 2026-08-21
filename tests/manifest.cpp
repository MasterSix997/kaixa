#include <test_support.hpp>

#include <kaixa/kaixa.hpp>
#include <kaixa/plugin/bundle.hpp>

#include <cstddef>
#include <ranges>
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
            manifest->dependencies.front().selection.path->generic_string(), std::string("../math"), "dependency path"
        );
    }
}

KAIXA_TEST(manifest_normalizes_products_public_edges_features_and_inline_members) {
    const auto document = kaixa::parse_manifest_document_string(
        "[package]\n"
        "name = \"app\"\n"
        "resolver = \"cmake\"\n"
        "\n"
        "[package-set]\n"
        "name = \"workspace\"\n"
        "default = [\"app\"]\n"
        "\n"
        "[public-dependencies]\n"
        "graphics = \"^2\"\n"
        "\n"
        "[features]\n"
        "default = [\"full\"]\n"
        "full = { dependencies = [\"graphics\"], dependency-features = { graphics = [\"vulkan\"] } }\n"
        "\n"
        "[bin]\n"
        "sources = [\"main.cpp\"]\n"
        "\n"
        "[members.support]\n"
        "resolver = \"cmake\"\n"
        "\n"
        "[members.support.lib]\n"
        "type = \"interface\"\n"
        "public-include = [\"include\"]\n",
        "product-model.toml"
    );

    context.check(document.has_value(), "product-model document parses");
    if (!document) {
        context.fail(kaixa::format_diagnostic(document.error()));
        return;
    }
    context.check_equal(document->inline_members.size(), std::size_t{1}, "inline member count");
    context.check_equal(document->package->products.size(), std::size_t{1}, "root product count");
    context.check_equal(document->package->default_features.front(), std::string("full"), "default feature");
    context.check(
        document->package->dependencies.front().visibility == kaixa::DependencyVisibility::public_dependency,
        "public dependency visibility is retained"
    );
    context.check_equal(
        document->package->features.front().dependency_features.at("graphics").front(),
        std::string("vulkan"),
        "dependency feature request is normalized"
    );
}

KAIXA_TEST(manifest_tree_loads_target_layers_and_advanced_target_data) {
    const TempDirectory root("manifest-tree");
    root.write(
        "Kaixa.toml",
        "[package-set]\n"
        "members = [\"app\"]\n"
        "default = [\"app\"]\n"
    );
    root.write(
        "app/Kaixa.toml",
        "[package]\n"
        "name = \"app\"\n"
        "resolver = \"cmake\"\n"
        "examples = [\"examples\"]\n"
    );
    root.write(
        "app/examples/Kaixa.toml",
        "[examples]\n"
        "name-template = \"demo.{stem}\"\n"
        "source = \"template.cpp\"\n"
        "required-features = { graphics = [\"vulkan\"] }\n"
        "matrix = { quality = [1, 2] }\n"
        "\n"
        "[[examples.resources]]\n"
        "from = \"assets\"\n"
        "to = \"assets\"\n"
    );

    const auto tree = kaixa::load_manifest_tree(root.path());
    context.check(tree.has_value(), "manifest tree loads");
    if (!tree) {
        context.fail(kaixa::format_diagnostic(tree.error()));
        return;
    }
    context.check_equal(tree->summary.documents, std::size_t{3}, "all documents are represented");
    context.check_equal(tree->summary.packages, std::size_t{1}, "package count");
    context.check_equal(tree->summary.package_sets, std::size_t{1}, "package-set count");
    context.check_equal(tree->summary.target_documents, std::size_t{1}, "target document count");
    const kaixa::PackageTarget& target = tree->target_documents.front().targets.front();
    context.check(target.matrix.has_value(), "target matrix is normalized");
    context.check_equal(target.resources.size(), std::size_t{1}, "target resources are normalized");
    context.check_equal(
        target.required_dependency_features.at("graphics").front(),
        std::string("vulkan"),
        "dependency feature requirement is normalized"
    );
}

KAIXA_TEST(manifest_rejects_unknown_keys) {
    const auto manifest = kaixa::parse_manifest_string(
        "[package]\nname = \"app\"\nresolver = \"cmake\"\ntypo = true\n", "invalid-manifest"
    );

    context.check(!manifest.has_value(), "unknown package key is rejected");
    if (!manifest) {
        context.check_contains(
            kaixa::format_diagnostic(manifest.error()), "package.typo", "diagnostic names the unknown key"
        );
    }
}

KAIXA_TEST(file_sets_expand_globs_and_keep_literal_generated_files) {
    const TempDirectory root("file-set");
    root.write("src/first.cpp", "");
    root.write("src/nested/second.cpp", "");
    root.write("src/generated/ignored.cpp", "");
    root.write("src/not-a-source.hpp", "");

    kaixa::FileSet files;
    files.include = {"src/**/*.cpp", "generated.cpp"};
    files.exclude = {"src/generated/**"};
    const auto expanded = kaixa::expand_file_set(files, root.path(), root.path());
    context.check(expanded.has_value(), "file set expands");
    if (!expanded) {
        context.fail(kaixa::format_diagnostic(expanded.error()));
        return;
    }

    context.check_equal(expanded->size(), std::size_t{3}, "matched and literal file count");
    context.check(
        std::ranges::find(*expanded, std::filesystem::path("src/first.cpp")) != expanded->end(),
        "direct match is included"
    );
    context.check(
        std::ranges::find(*expanded, std::filesystem::path("src/nested/second.cpp")) != expanded->end(),
        "recursive match is included"
    );
    context.check(
        std::ranges::find(*expanded, std::filesystem::path("generated.cpp")) != expanded->end(),
        "literal generated file is preserved"
    );
}

KAIXA_TEST(manifest_reads_inline_targets_and_external_target_references) {
    const auto manifest = kaixa::parse_manifest_string(
        "[package]\n"
        "name = \"app\"\n"
        "resolver = \"cmake\"\n"
        "tests = [\"tests\"]\n"
        "examples = [\"examples/*\"]\n"
        "\n"
        "[test]\n"
        "sources = [\"tests/*.cpp\"]\n"
        "discover = true\n"
        "\n"
        "[test.dependencies]\n"
        "support = { path = \"../support\" }\n"
        "\n"
        "[test.cmake]\n"
        "link-libraries = [\"support\"]\n"
        "\n"
        "[benchmarks]\n"
        "sources = [\"benchmarks/*.cpp\"]\n",
        "target-manifest"
    );

    context.check(manifest.has_value(), "target manifest parses");
    if (!manifest) {
        context.fail(kaixa::format_diagnostic(manifest.error()));
        return;
    }

    context.check_equal(manifest->target_references.size(), std::size_t{2}, "reference count");
    context.check_equal(manifest->targets.size(), std::size_t{2}, "inline target group count");
    context.check(manifest->targets[0].discover, "test discovery is retained");
    context.check(!manifest->targets[0].each_source, "singular test is grouped");
    context.check(manifest->targets[1].each_source, "plural benchmarks are per source");
    context.check(manifest->targets[0].resolver_options.has_value(), "resolver options are retained");
    context.check_equal(manifest->targets[0].dependencies.size(), std::size_t{1}, "target dependency count");
    context.check_equal(
        manifest->targets[0].dependencies.front().request.package, std::string("support"), "target dependency name"
    );
}

KAIXA_TEST(package_target_names_may_preserve_resolver_namespaces) {
    const auto manifest = kaixa::parse_manifest_string(
        "[package]\n"
        "name = \"target_names\"\n"
        "resolver = \"cmake\"\n"
        "\n"
        "[test]\n"
        "name = \"threading.tests.noexcept\"\n"
        "sources = [\"test.cpp\"]\n",
        "target-name-test"
    );
    context.check(manifest.has_value(), "dotted target name parses");
    if (!manifest) {
        context.fail(kaixa::format_diagnostic(manifest.error()));
        return;
    }

    const auto formatted = kaixa::format_manifest(*manifest);
    context.check(formatted.has_value(), "dotted target name formats");
    if (!formatted)
        context.fail(kaixa::format_diagnostic(formatted.error()));
}

KAIXA_TEST(package_targets_preserve_feature_requirements_for_selection) {
    const TempDirectory root("target-features");
    root.write(
        "Kaixa.toml",
        "[package]\n"
        "name = \"app\"\n"
        "resolver = \"cmake\"\n"
        "\n"
        "[example]\n"
        "sources = [\"example.cpp\"]\n"
        "required-features = [\"graphics\"]\n"
    );
    root.write("example.cpp", "int main() { return 0; }\n");

    const auto graph = kaixa::load_workspace(root.path());
    context.check(graph.has_value(), "feature requirement is normalized without activating it");
    if (!graph)
        return;
    const kaixa::PackageNode& app = graph->nodes().front();
    context.check(app.manifest.has_value(), "managed package retains its manifest");
    if (app.manifest && !app.manifest->resolved_targets.empty()) {
        context.check_equal(
            app.manifest->resolved_targets.front().required_features.front(),
            std::string("graphics"),
            "selection requirement is retained"
        );
    }
}

KAIXA_TEST(programmatic_manifest_formats_and_round_trips) {
    kaixa::Manifest authored{"app", "cmake"};
    authored.version = kaixa::Version{"0.1.0"};
    authored.dependencies.emplace_back("math", "../math");
    authored.target_references.push_back({kaixa::PackageTargetKind::test, "tests", {}});
    kaixa::PackageTarget examples;
    examples.kind = kaixa::PackageTargetKind::example;
    examples.each_source = true;
    examples.sources.include = {"examples/*.cpp"};
    examples.dependencies.emplace_back("support", "../support");
    examples.resolver_options = kaixa::Value::table({{"link-libraries", kaixa::Value::array({"app"})}});
    authored.targets.push_back(std::move(examples));
    authored.configurations.defaults.push_back("dev");

    kaixa::ConfigurationDefinition dev;
    dev.name = "dev";
    dev.profile = "debug";
    dev.resolvers.push_back(
        {"cmake", kaixa::Value::table({{"generator", "Ninja"}, {"arguments", kaixa::Value::array({"-DDEV=ON"})}})}
    );
    authored.configurations.definitions.push_back(std::move(dev));
    authored.resolver_options = kaixa::Value::table(
        {{"source", "project"},
         {"cxx-standard", 23},
         {"target",
          kaixa::Value::array({kaixa::Value::table(
              {{"name", "app"}, {"type", "executable"}, {"sources", kaixa::Value::array({"src/main.cpp"})}}
          )})}}
    );

    const auto text = kaixa::format_manifest(authored);
    context.check(text.has_value(), "programmatic manifest formats");
    if (!text) {
        context.fail(kaixa::format_diagnostic(text.error()));
        return;
    }
    context.check_contains(*text, "[[config]]", "canonical configuration syntax");
    context.check_contains(*text, "cmake.generator = \"Ninja\"", "resolver configuration uses dotted keys");
    context.check(!text->contains("[config.cmake]"), "resolver configuration avoids repeated table headers");
    context.check_contains(*text, "[[cmake.target]]", "canonical target syntax");
    context.check_contains(*text, "tests = [\"tests\"]", "target manifest references format");
    context.check_contains(*text, "[examples]", "package targets format");
    context.check_contains(*text, "[examples.dependencies]", "package target dependencies format");

    const auto parsed = kaixa::parse_manifest_string(*text, "generated-manifest");
    context.check(parsed.has_value(), "formatted manifest parses");
    if (!parsed) {
        context.fail(kaixa::format_diagnostic(parsed.error()));
        return;
    }
    context.check_equal(parsed->name, authored.name, "package name round-trips");
    context.check_equal(parsed->dependencies.front().request.package, std::string("math"), "dependency round-trips");
    context.check_equal(parsed->configurations.defaults.front(), std::string("dev"), "configuration round-trips");
    context.check(parsed->resolver_options.has_value(), "resolver options round-trip");
    context.check_equal(parsed->target_references.size(), std::size_t{1}, "target reference round-trips");
    context.check_equal(parsed->targets.size(), std::size_t{1}, "package target round-trips");
    context.check_equal(parsed->targets.front().dependencies.size(), std::size_t{1}, "target dependency round-trips");
}

KAIXA_TEST(programmatic_manifest_rejects_invalid_data) {
    kaixa::Manifest manifest{"app", "cmake"};
    manifest.dependencies.emplace_back("math", "first");
    manifest.dependencies.emplace_back("math", "second");

    const auto text = kaixa::format_manifest(manifest);
    context.check(!text.has_value(), "invalid programmatic manifest is rejected");
    if (!text) {
        context.check_contains(
            kaixa::format_diagnostic(text.error()), "duplicate dependency `math`", "diagnostic explains invalid model"
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

    const kaixa::ExtensionRegistry registry = kaixa::plugin::default_registry();
    const kaixa::BuildEnvironment environment{root.path(), root.path() / "out", "debug"};
    const auto plan = kaixa::plan_build(*graph, registry, environment);
    context.check(plan.has_value(), "CMake resolver creates a plan");
    if (!plan)
        return;

    context.check_equal(plan->actions().size(), std::size_t{2}, "one composed configure and build");
    context.check_equal(
        plan->generated_files().size(), std::size_t{3}, "variant metadata, integration and File API query"
    );
    if (plan->actions().size() == 2) {
        context.check(plan->actions()[0].stage == kaixa::ActionStage::synchronize, "configure synchronizes");
        context.check(plan->actions()[1].stage == kaixa::ActionStage::build, "build stays explicit");
    }
}

KAIXA_TEST(graph_rejects_dependency_cycles) {
    kaixa::Graph graph;
    const kaixa::PackageId first =
        graph.add({{}, "first", {}, kaixa::PackageKind::managed, "cmake", std::nullopt, {}, {}, {}});
    const kaixa::PackageId second =
        graph.add({{}, "second", {}, kaixa::PackageKind::managed, "cmake", std::nullopt, {}, {}, {}});
    graph[first].dependencies.push_back(second);
    graph[second].dependencies.push_back(first);

    const auto order = graph.build_order();
    context.check(!order.has_value(), "cycle is rejected");
    if (!order)
        context.check_contains(order.error().message, "first -> second -> first", "cycle path");
}
