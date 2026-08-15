#include <test_support.hpp>

#include <kaixa/kaixa.hpp>

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

KAIXA_TEST(dependencies_normalize_versions_aliases_and_source_drivers) {
    const auto manifest = kaixa::parse_manifest_string(
        "[package]\n"
        "name = \"app\"\n"
        "resolver = \"cmake\"\n"
        "\n"
        "[dependencies]\n"
        "math = \"0.4\"\n"
        "engine_physics = { version = \"^2.1\", alias = \"physics\", features = [\"debug\"] }\n"
        "engine = { git = { url = \"https://example.invalid/engine.git\", tag = \"v0.4.0\" } }\n",
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
    context.check_equal(manifest->dependencies[2].selection.source->driver, std::string("git"), "source driver");
    context.check(
        manifest->dependencies[2].selection.source->options.find("url") != nullptr,
        "source options remain opaque"
    );

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
    context.check_equal(
        defaults->graph[defaults->graph.roots().front()].name,
        std::string("editor"),
        "default order is preserved"
    );
    context.check_equal(defaults->available.candidates().size(), std::size_t{3}, "all candidates remain observable");

    const std::vector<std::string> selected_names{"game_runner"};
    const auto selected = kaixa::resolve_workspace(root.path(), selected_names);
    context.check(selected.has_value(), "explicit package resolves");
    if (!selected)
        return;

    context.check_equal(selected->graph.roots().size(), std::size_t{1}, "one explicit root");
    context.check_equal(selected->graph.size(), std::size_t{1}, "unreached packages stay outside the graph");
    context.check_equal(
        selected->graph[selected->graph.roots().front()].name,
        std::string("game_runner"),
        "explicit root replaces defaults"
    );
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
        context.check_contains(
            kaixa::format_diagnostic(duplicate.error()),
            "selected more than once",
            "duplicate diagnostic"
        );
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
        context.check_contains(
            kaixa::format_diagnostic(graph.error()),
            "does not satisfy `^2`",
            "diagnostic reports the candidate version"
        );
    }
}

KAIXA_TEST(package_index_exposes_nested_candidates_without_loading_the_graph) {
    const TempDirectory root("nested-package-set");
    root.write(
        "Kaixa.toml",
        "[package-set]\n"
        "members = [\"engine\"]\n"
        "default = [\"engine\"]\n"
    );
    root.write(
        "engine/Kaixa.toml",
        "[package]\n"
        "name = \"engine\"\n"
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
        "engine/libraries/math/Kaixa.toml",
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
    const std::filesystem::path engine_manifest = std::filesystem::canonical(root.path() / "engine/Kaixa.toml");
    const kaixa::LocalPackageCandidate* math = index->find_for(engine_manifest, "math");
    context.check(math != nullptr, "nested package resolves within its own set");
    if (math)
        context.check_equal(math->version->text, std::string("0.4.1"), "candidate metadata is retained");
}
