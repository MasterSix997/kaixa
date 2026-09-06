#include <test_support.hpp>

#include <kaixa/kaixa.hpp>
#include <kaixa/plugin/bundle.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

using kaixa::testing::TempDirectory;

KAIXA_TEST(manifest_document_distinguishes_packages_and_package_sets) {
    const auto document = kaixa::parse_manifest_document_string(
        "[package-set]\n"
        "members = [\"libraries/*\", \"editor\"]\n"
        "exclude = [\"libraries/legacy\"]\n"
        "default = [\"editor\"]\n",
        "package-set.toml"
    );

    context.check(document.has_value(), "pure package set parses");
    if (!document) {
        context.fail(kaixa::format_diagnostic(document.error()));
        return;
    }

    context.check(!document->package.has_value(), "document has no package");
    context.check(document->package_set.has_value(), "document has a package set");
    context.check_equal(document->package_set->members.size(), std::size_t{2}, "member pattern count");
    context.check_equal(document->package_set->defaults.front(), std::string("editor"), "default package");
}

KAIXA_TEST(operational_resolution_loads_only_reachable_manifests) {
    const TempDirectory root("reachable-manifests");
    root.write(
        "Kaixa.toml",
        "[package-set]\n"
        "members = [\"packages/*\"]\n"
        "default = [\"app\"]\n"
    );
    root.write(
        "packages/app/Kaixa.toml",
        "[package]\n"
        "name = \"app\"\n"
        "resolver = \"cmake\"\n"
    );
    root.write("unrelated/Kaixa.toml", "[unrelated]\nvalue = true\n");

    kaixa::ResolutionOptions operational;
    operational.write_lock = false;
    operational.refresh_sources = false;
    operational.load_model = false;
    const auto resolved = kaixa::resolve_workspace(root.path(), operational);
    context.check(resolved.has_value(), "operational resolution ignores unrelated manifest trees");
    if (resolved)
        context.check(resolved->model.documents.empty(), "operational resolution does not retain the authored model");

    kaixa::ResolutionOptions authored;
    authored.write_lock = false;
    authored.refresh_sources = false;
    const auto validated = kaixa::resolve_workspace(root.path(), authored);
    context.check(!validated.has_value(), "authored model loading still validates every manifest document");
}

KAIXA_TEST(dependencies_normalize_versions_aliases_and_source_drivers) {
    const auto manifest = kaixa::parse_manifest_string(
        "[package]\n"
        "name = \"app\"\n"
        "resolver = \"cmake\"\n"
        "\n"
        "[dependencies]\n"
        "math = \"0.4\"\n"
        "component_physics = { version = \"^2.1\", alias = \"physics\", features = [\"debug\"] }\n"
        "component = { git = { url = \"https://example.invalid/component.git\", tag = \"v0.4.0\" } }\n",
        "dependencies.toml"
    );

    context.check(manifest.has_value(), "dependency forms parse");
    if (!manifest) {
        context.fail(kaixa::format_diagnostic(manifest.error()));
        return;
    }

    context.check_equal(manifest->dependencies.size(), std::size_t{3}, "dependency count");
    context.check_equal(manifest->dependencies[0].request.version->text, std::string("0.4"), "compact version");
    context.check_equal(*manifest->dependencies[1].alias, std::string("physics"), "inline alias");
    context.check_equal(manifest->dependencies[1].local_name(), std::string_view("physics"), "local binding name");
    context.check_equal(manifest->dependencies[2].selection.source()->driver, std::string("git"), "source driver");
    context.check(manifest->dependencies[2].selection.source()->options.find("url") != nullptr, "source options remain opaque");

    const auto formatted = kaixa::format_manifest(*manifest);
    context.check(formatted.has_value(), "normalized dependencies format");
    if (formatted) {
        context.check_contains(*formatted, "math = \"0.4\"", "compact form is retained");
        context.check_contains(*formatted, "alias = \"physics\"", "alias is written inline");
        context.check_contains(*formatted, "git = { url =", "driver table is written inline");
    }
}

KAIXA_TEST(version_requirements_follow_semver_compatibility) {
    const auto compatible = kaixa::parse_version_requirement("0.4");
    const auto wildcard = kaixa::parse_version_requirement("1.2.*");
    const auto stable = kaixa::parse_version("0.4.9");
    const auto next_minor = kaixa::parse_version("0.5.0");
    const auto wildcard_match = kaixa::parse_version("1.2.7");

    context.check(compatible && wildcard && stable && next_minor && wildcard_match, "versions parse");
    if (!compatible || !wildcard || !stable || !next_minor || !wildcard_match)
        return;

    context.check(kaixa::matches(*compatible, *stable), "compatible zero-major version matches");
    context.check(!kaixa::matches(*compatible, *next_minor), "next zero-major minor is incompatible");
    context.check(kaixa::matches(*wildcard, *wildcard_match), "wildcard requirement matches");
}

KAIXA_TEST(package_names_accept_dots_but_not_path_separators) {
    context.check(kaixa::is_valid_package_name("ecs.reflection"), "dotted package name is valid");
    context.check(!kaixa::is_valid_package_name("company/tools"), "package name is not a path");
}

KAIXA_TEST(package_set_expands_members_and_resolves_a_default_root) {
    const TempDirectory root("package-set-default");
    root.write(
        "Kaixa.toml",
        "[package-set]\n"
        "members = [\"packages/*\"]\n"
        "exclude = [\"packages/legacy\"]\n"
        "default = [\"app\"]\n"
    );
    root.write(
        "packages/app/Kaixa.toml",
        "[package]\n"
        "name = \"app\"\n"
        "version = \"1.0.0\"\n"
        "resolver = \"cmake\"\n"
        "\n"
        "[dependencies]\n"
        "math = \"0.4\"\n"
    );
    root.write(
        "packages/math/Kaixa.toml",
        "[package]\n"
        "name = \"math\"\n"
        "version = \"0.4.3\"\n"
        "resolver = \"cmake\"\n"
    );
    root.write(
        "packages/legacy/Kaixa.toml",
        "[package]\n"
        "name = \"math\"\n"
        "version = \"0.4.0\"\n"
        "resolver = \"cmake\"\n"
    );

    const auto graph = kaixa::load_workspace(root.path());
    context.check(graph.has_value(), "package set loads");
    if (!graph) {
        context.fail(kaixa::format_diagnostic(graph.error()));
        return;
    }

    context.check_equal(graph->size(), std::size_t{2}, "only reached packages enter the graph");
    const kaixa::PackageId root_package = graph->roots().front();
    context.check_equal((*graph)[root_package].name, std::string("app"), "default package is the root");
    const kaixa::PackageId math = (*graph)[root_package].dependencies.front();
    context.check_equal((*graph)[math].name, std::string("math"), "local candidate resolves");
}

KAIXA_TEST(package_set_supports_multiple_defaults_and_explicit_roots) {
    const TempDirectory root("package-set-roots");
    root.write(
        "Kaixa.toml",
        "[package-set]\n"
        "members = [\"packages/*\"]\n"
        "default = [\"editor\", \"game_runner\"]\n"
    );
    root.write(
        "packages/editor/Kaixa.toml",
        "[package]\n"
        "name = \"editor\"\n"
        "resolver = \"cmake\"\n"
        "\n"
        "[dependencies]\n"
        "math = \"1\"\n"
    );
    root.write(
        "packages/game_runner/Kaixa.toml",
        "[package]\n"
        "name = \"game_runner\"\n"
        "resolver = \"cmake\"\n"
    );
    root.write(
        "packages/math/Kaixa.toml",
        "[package]\n"
        "name = \"math\"\n"
        "version = \"1.0.0\"\n"
        "resolver = \"cmake\"\n"
    );

    const auto defaults = kaixa::resolve_workspace(root.path());
    context.check(defaults.has_value(), "multiple default packages resolve");
    if (!defaults)
        return;

    context.check_equal(defaults->graph.roots().size(), std::size_t{2}, "default root count");
    context.check_equal(defaults->graph[defaults->graph.roots().front()].name, std::string("editor"), "default order is preserved");
    context.check_equal(defaults->available.candidates().size(), std::size_t{3}, "all candidates remain observable");

    const std::vector<std::string> selected_names{"game_runner"};
    const auto selected = kaixa::resolve_workspace(root.path(), selected_names);
    context.check(selected.has_value(), "explicit package resolves");
    if (!selected)
        return;

    context.check_equal(selected->graph.roots().size(), std::size_t{1}, "one explicit root");
    context.check_equal(selected->graph.size(), std::size_t{1}, "unreached packages stay outside the graph");
    context
        .check_equal(selected->graph[selected->graph.roots().front()].name, std::string("game_runner"), "explicit root replaces defaults");
}

KAIXA_TEST(package_selection_reports_duplicates_and_available_names) {
    const TempDirectory root("package-selection-errors");
    root.write(
        "Kaixa.toml",
        "[package-set]\n"
        "members = [\"packages/*\"]\n"
        "default = [\"app\"]\n"
    );
    root.write(
        "packages/app/Kaixa.toml",
        "[package]\n"
        "name = \"app\"\n"
        "resolver = \"cmake\"\n"
    );

    const std::vector<std::string> duplicate_names{"app", "app"};
    const auto duplicate = kaixa::resolve_workspace(root.path(), duplicate_names);
    context.check(!duplicate.has_value(), "duplicate package selection is rejected");
    if (!duplicate) {
        context.check_contains(kaixa::format_diagnostic(duplicate.error()), "selected more than once", "duplicate diagnostic");
    }

    const std::vector<std::string> missing_names{"missing"};
    const auto missing = kaixa::resolve_workspace(root.path(), missing_names);
    context.check(!missing.has_value(), "unknown package selection is rejected");
    if (!missing) {
        const std::string diagnostic = kaixa::format_diagnostic(missing.error());
        context.check_contains(diagnostic, "package `missing` is not available", "missing diagnostic");
        context.check_contains(diagnostic, "available packages: app", "available package note");
    }
}

KAIXA_TEST(package_set_reports_an_incompatible_nearest_candidate) {
    const TempDirectory root("package-set-version");
    root.write(
        "Kaixa.toml",
        "[package]\n"
        "name = \"app\"\n"
        "resolver = \"cmake\"\n"
        "\n"
        "[package-set]\n"
        "members = [\"packages/*\"]\n"
        "\n"
        "[dependencies]\n"
        "math = \"^2\"\n"
    );
    root.write(
        "packages/math/Kaixa.toml",
        "[package]\n"
        "name = \"math\"\n"
        "version = \"1.5.0\"\n"
        "resolver = \"cmake\"\n"
    );

    const auto graph = kaixa::load_workspace(root.path());
    context.check(!graph.has_value(), "incompatible local candidate is rejected");
    if (!graph) {
        context
            .check_contains(kaixa::format_diagnostic(graph.error()), "does not satisfy `^2`", "diagnostic reports the candidate version");
    }
}

KAIXA_TEST(package_index_exposes_nested_candidates_without_loading_the_graph) {
    const TempDirectory root("nested-package-set");
    root.write(
        "Kaixa.toml",
        "[package-set]\n"
        "members = [\"component\"]\n"
        "default = [\"component\"]\n"
    );
    root.write(
        "component/Kaixa.toml",
        "[package]\n"
        "name = \"component\"\n"
        "version = \"1.0.0\"\n"
        "resolver = \"cmake\"\n"
        "\n"
        "[package-set]\n"
        "members = [\"libraries/*\"]\n"
        "\n"
        "[dependencies]\n"
        "math = \"0.4\"\n"
    );
    root.write(
        "component/libraries/math/Kaixa.toml",
        "[package]\n"
        "name = \"math\"\n"
        "version = \"0.4.1\"\n"
        "resolver = \"cmake\"\n"
    );

    const std::filesystem::path root_manifest = std::filesystem::canonical(root.path() / "Kaixa.toml");
    const auto document = kaixa::parse_manifest_document_file(root_manifest);
    context.check(document.has_value(), "root package set parses");
    if (!document)
        return;

    const auto index = kaixa::PackageIndex::discover(root_manifest, *document);
    context.check(index.has_value(), "nested package index loads");
    if (!index) {
        context.fail(kaixa::format_diagnostic(index.error()));
        return;
    }

    context.check_equal(index->candidates().size(), std::size_t{2}, "nested candidates are observable");
    const std::filesystem::path component_manifest = std::filesystem::canonical(root.path() / "component/Kaixa.toml");
    const kaixa::LocalPackageCandidate* math = index->find_for(component_manifest, "math");
    context.check(math != nullptr, "nested package resolves within its own set");
    if (math)
        context.check_equal(math->version->text, std::string("0.4.1"), "candidate metadata is retained");
}

KAIXA_TEST(target_policy_creates_a_separate_configured_package_instance) {
    const TempDirectory root("configured-instances");
    root.write(
        "Kaixa.toml",
        "[package]\n"
        "name = \"app\"\n"
        "resolver = \"cmake\"\n"
        "\n"
        "[package-set]\n"
        "\n"
        "[package-set.policy]\n"
        "cxx = 23\n"
        "\n"
        "[lib]\n"
        "type = \"static\"\n"
        "sources = [\"app.cpp\"]\n"
        "\n"
        "[[test]]\n"
        "name = \"app.tests.noexcept\"\n"
        "sources = [\"test.cpp\"]\n"
        "policy = { exceptions = false }\n"
    );
    root.write("app.cpp", "int answer() { return 42; }\n");
    root.write("test.cpp", "int main() { return 0; }\n");

    kaixa::ExtensionRegistry extensions = kaixa::plugin::default_registry();
    const auto resolution = kaixa::resolve_workspace(root.path(), kaixa::ResolutionOptions{{}, &extensions});
    context.check(resolution.has_value(), "configured workspace resolves");
    if (!resolution) {
        context.fail(kaixa::format_diagnostic(resolution.error()));
        return;
    }
    context.check_equal(resolution->instances.size(), std::size_t{2}, "default and target instances");
    context.check(resolution->instances[0].artifact.starts_with("app-"), "default artifact has a stable package prefix");
    context.check(resolution->instances[1].artifact.starts_with("app-"), "policy-specific artifact has a stable package prefix");
    context.check(
        resolution->instances[0].artifact != resolution->instances[1].artifact,
        "different effective policies have different artifacts"
    );
    context.check_equal(resolution->instances[0].contexts.front(), std::string("app:default"), "default context is retained");
    context.check_equal(resolution->instances[1].contexts.front(), std::string("app.tests.noexcept"), "target context is retained");
    context.check_equal(resolution->instances[1].policy_layers.size(), std::size_t{2}, "package-set and target policies compose");
    const kaixa::PolicySetting* exceptions = resolution->instances[1].policy.find("exceptions");
    context.check(exceptions && !*exceptions->value.as_boolean(), "target ABI policy is effective");
}

KAIXA_TEST(configured_features_activate_optional_dependencies) {
    const TempDirectory root("configured-features");
    root.write(
        "Kaixa.toml",
        "[package-set]\n"
        "members = [\"app\", \"tool\"]\n"
        "default = [\"app\"]\n"
    );
    root.write(
        "app/Kaixa.toml",
        "[package]\n"
        "name = \"app\"\n"
        "resolver = \"cmake\"\n"
        "\n"
        "[features]\n"
        "tools = { dependencies = [\"tool\"] }\n"
        "\n"
        "[dependencies]\n"
        "tool = { version = \"1\", optional = true }\n"
    );
    root.write(
        "tool/Kaixa.toml",
        "[package]\n"
        "name = \"tool\"\n"
        "version = \"1.0.0\"\n"
        "resolver = \"cmake\"\n"
    );

    const kaixa::Value feature_settings = kaixa::Value::table({{"app", kaixa::Value::array({kaixa::Value("tools")})}});
    const auto resolution = kaixa::resolve_workspace(root.path(), kaixa::ResolutionOptions{{}, nullptr, {}, {}, &feature_settings});
    context.check(resolution.has_value(), "configured feature graph resolves");
    if (!resolution) {
        context.fail(kaixa::format_diagnostic(resolution.error()));
        return;
    }
    context.check_equal(resolution->graph.size(), std::size_t{2}, "optional dependency is loaded");
    const auto app = resolution->graph.find_by_name("app");
    context.check(app.has_value(), "configured package exists");
    if (app) {
        context.check_equal(resolution->graph[*app].active_features.front(), std::string("tools"), "configured feature is active");
    }
}

KAIXA_TEST(root_package_rejects_a_redundant_package_set_default) {
    const TempDirectory root("package-set-redundant-default");
    root.write(
        "Kaixa.toml",
        "[package]\n"
        "name = \"app\"\n"
        "version = \"1.0.0\"\n"
        "resolver = \"cmake\"\n"
        "\n"
        "[package-set]\n"
        "name = \"app\"\n"
        "default = [\"app\"]\n"
    );

    const auto document = kaixa::parse_manifest_document_file(root.path() / "Kaixa.toml");
    context.check(!document.has_value(), "declaring a default next to `[package]` is rejected");
    if (!document) {
        context.check_contains(document.error().message, "`[package]` is already the root", "redundant default diagnostic");
        context.check(document.error().location.has_value(), "diagnostic carries a location");
        if (document.error().location)
            context.check_contains(document.error().location->config_path, "default", "diagnostic points at the `default` key");
    }
}

KAIXA_TEST(root_package_accepts_a_package_set_without_a_default) {
    const TempDirectory root("package-set-root-package");
    root.write(
        "Kaixa.toml",
        "[package]\n"
        "name = \"app\"\n"
        "version = \"1.0.0\"\n"
        "resolver = \"cmake\"\n"
        "\n"
        "[package-set]\n"
        "name = \"app\"\n"
        "members = [\"packages/*\"]\n"
    );
    root.write(
        "packages/math/Kaixa.toml",
        "[package]\n"
        "name = \"math\"\n"
        "version = \"0.4.3\"\n"
        "resolver = \"cmake\"\n"
    );

    const auto graph = kaixa::load_workspace(root.path());
    context.check(graph.has_value(), "a root package may still declare set members");
    if (!graph) {
        context.fail(kaixa::format_diagnostic(graph.error()));
        return;
    }

    context.check_equal((*graph)[graph->roots().front()].name, std::string("app"), "`[package]` remains the root");
}
