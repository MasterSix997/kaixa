#include <test_support.hpp>

#include <kaixa/foundation/filesystem.hpp>
#include <kaixa/foundation/process.hpp>
#include <kaixa/kaixa.hpp>
#include <kaixa/plugin/bundle.hpp>

#include <filesystem>
#include <string>

using kaixa::testing::TempDirectory;

namespace {
    class RecordingPublicationBackend final : public kaixa::PublicationBackend {
    public:
        [[nodiscard]] kaixa::Result<void> upload(const kaixa::PackageUpload& request) const override {
            uploaded = true;
            archived = std::filesystem::is_regular_file(request.archive);
            return {};
        }

        mutable bool archived = false;
        mutable bool uploaded = false;
    };

    kaixa::Result<void> run(std::vector<std::string> arguments, const std::filesystem::path& directory) {
        auto result = kaixa::run_process({std::move(arguments), directory, {}, kaixa::ProcessOutputMode::capture});
        if (!result)
            return std::unexpected(result.error());

        if (!result->succeeded())
            return std::unexpected(kaixa::error("fixture command failed").add_note(result->output));

        return {};
    }
}

KAIXA_TEST(publication_transport_is_replaceable) {
    const TempDirectory root("publication-backend");
    root.write(
        "library/Kaixa.toml",
        "[package]\n"
        "name = \"replaceable\"\n"
        "version = \"1.0.0\"\n"
        "resolver = \"cmake\"\n"
    );
    const RecordingPublicationBackend backend;
    const auto local = kaixa::publish_package({root.path() / "library", root.path() / "registry"}, &backend);
    context.check(local.has_value(), "local publication needs no transport backend");
    context.check(!backend.uploaded, "local registries do not invoke remote upload");

    kaixa::PublishRequest remote{root.path() / "library", root.path() / "registry"};
    remote.endpoint = "https://registry.invalid";
    const auto published = kaixa::publish_package(remote, &backend);
    context.check(published.has_value(), "publication accepts an injected transport backend");
    context.check(backend.uploaded, "endpoint publication uses the injected transport");
    context.check(backend.archived, "the core builds the archive handed to the transport");

    const auto missing = kaixa::publish_package(remote);
    context.check(!missing.has_value(), "endpoint publication without a transport backend is rejected");
    if (!missing)
        context.check_contains(missing.error().message, "requires a publication backend", "missing transport diagnostic");
}

KAIXA_TEST(sha256_matches_the_standard_test_vector) {
    context.check_equal(
        kaixa::sha256("abc"),
        std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
        "SHA-256 digest"
    );
}

KAIXA_TEST(source_cache_is_transactional_and_available_offline) {
    const TempDirectory root("source-cache");
    std::size_t populations = 0;
    const kaixa::SourceContext online{root.path(), root.path() / "cache"};
    auto first = kaixa::materialize_source_cache(
        online,
        "test",
        "stable locator",
        [&](const std::filesystem::path& destination) -> kaixa::Result<kaixa::SourceTree> {
            ++populations;
            auto written = kaixa::write_file(destination / "Kaixa.toml", "[package]\nname = \"cached\"\nresolver = \"cmake\"\n");
            if (!written)
                return std::unexpected(written.error());

            return kaixa::SourceTree{destination, "identity", "sha256:test"};
        }
    );
    context.check(first.has_value(), "first source population succeeds");
    if (!first)
        return;

    const kaixa::SourceContext offline{root.path(), root.path() / "cache", true};
    auto second = kaixa::materialize_source_cache(
        offline,
        "test",
        "stable locator",
        [&](const std::filesystem::path&) -> kaixa::Result<kaixa::SourceTree> {
            ++populations;
            return std::unexpected(kaixa::error("offline cache should not populate"));
        }
    );
    context.check(second.has_value(), "cached source opens offline");
    context.check_equal(populations, std::size_t{1}, "cache is populated exactly once");
    if (second) {
        context.check(second->cache_hit, "second materialization is a cache hit");
        context.check_equal(second->identity.value_or(""), std::string("identity"), "identity survives cache metadata");
    }
}

KAIXA_TEST(frozen_cache_miss_and_invalid_download_hash_are_rejected) {
    const TempDirectory root("source-verification");
    const kaixa::SourceContext offline{root.path(), root.path() / "cache", true};
    auto missing = kaixa::materialize_source_cache(
        offline,
        "test",
        "missing locator",
        [](const std::filesystem::path&) -> kaixa::Result<kaixa::SourceTree> {
            return std::unexpected(kaixa::error("unreachable population"));
        }
    );
    context.check(!missing.has_value(), "frozen cache miss fails");
    if (!missing)
        context.check_contains(missing.error().message, "frozen mode", "offline diagnostic explains the policy");

    root.write("remote.toml", "[package]\nname = \"remote\"\nresolver = \"cmake\"\n");
    kaixa::ExtensionRegistry registry = kaixa::plugin::default_registry();
    kaixa::SourceDriver* driver = registry.find_source_driver("url");
    context.check(driver != nullptr, "URL driver is registered");
    if (!driver)
        return;

    const kaixa::SourceLocator source{"url",
        kaixa::Value::table({{"url", "file:///" + (root.path() / "remote.toml").generic_string()}, {"sha256", std::string(64, '0')}})};
    auto materialized = driver->materialize(source, {root.path(), root.path() / "cache"});
    context.check(!materialized.has_value(), "URL source rejects a mismatched SHA-256");
    if (!materialized)
        context.check_contains(materialized.error().message, "SHA-256", "integrity diagnostic names SHA-256");
}

KAIXA_TEST(git_source_pins_a_commit_and_reuses_it_offline) {
    const TempDirectory root("git-source");
    root.write("repository/Kaixa.toml", "[package]\nname = \"git_package\"\nversion = \"1.0.0\"\nresolver = \"cmake\"\n");
    auto initialized = run({"git", "init", "--quiet"}, root.path() / "repository");
    auto added = run({"git", "add", "Kaixa.toml"}, root.path() / "repository");
    auto committed = run(
        {"git", "-c", "user.name=Kaixa Tests", "-c", "user.email=tests@kaixa.invalid", "commit", "--quiet", "-m", "fixture"},
        root.path() / "repository"
    );
    context.check(initialized.has_value() && added.has_value() && committed.has_value(), "local Git fixture is created");
    if (!initialized || !added || !committed)
        return;

    kaixa::ExtensionRegistry registry = kaixa::plugin::default_registry();
    kaixa::SourceDriver* driver = registry.find_source_driver("git");
    context.check(driver != nullptr, "Git driver is registered");
    if (!driver)
        return;

    const std::filesystem::path repository = root.path() / "repository";
    const kaixa::SourceLocator source{"git", kaixa::Value::table({{"url", repository.generic_string()}, {"rev", "HEAD"}})};
    auto online = driver->materialize(source, {root.path(), root.path() / "cache"});
    context.check(online.has_value() && online->has_value(), "Git source materializes");
    if (!online || !*online)
        return;

    context.check((**online).identity && (**online).identity->size() == 40, "Git source returns a concrete commit");
    auto offline = driver->materialize(source, {root.path(), root.path() / "cache", true});
    context.check(offline.has_value() && offline->has_value(), "Git source reopens in frozen mode");
    if (offline && *offline)
        context.check((**offline).cache_hit, "frozen Git source uses cache");
}

KAIXA_TEST(published_source_is_searchable_downloadable_and_lockable) {
    const TempDirectory root("published-source");
    root.write(
        "library/Kaixa.toml",
        "[package]\n"
        "name = \"downloaded\"\n"
        "version = \"1.2.3\"\n"
        "resolver = \"cmake\"\n"
    );
    auto published = kaixa::publish_package({root.path() / "library", root.path() / "registry"});
    context.check(published.has_value(), "source package publishes");
    if (!published) {
        context.fail(kaixa::format_diagnostic(published.error()));
        return;
    }

    root.write(
        "workspace/Kaixa.toml",
        "[package]\n"
        "name = \"application\"\n"
        "resolver = \"cmake\"\n"
        "\n"
        "[dependencies]\n"
        "downloaded = \"^1.0\"\n"
        "\n"
        "[[provider]]\n"
        "name = \"local\"\n"
        "driver = \"kaixa-registry\"\n"
        "default = true\n"
        "index = \"../registry/index.toml\"\n"
    );

    kaixa::ExtensionRegistry registry = kaixa::plugin::default_registry();
    auto resolved = kaixa::resolve_workspace(
        root.path() / "workspace",
        kaixa::ResolutionOptions{{}, &registry, root.path() / "cache", {}, nullptr, {}, kaixa::LockMode::update}
    );
    context.check(resolved.has_value(), "published package resolves and downloads");
    if (!resolved) {
        context.fail(kaixa::format_diagnostic(resolved.error()));
        return;
    }
    const auto package = resolved->graph.find_by_name("downloaded");
    context.check(package.has_value(), "downloaded package enters the graph");
    if (package && resolved->graph[*package].source) {
        context.check_equal(
            resolved->graph[*package].source->integrity.value_or(""),
            "sha256:" + published->integrity,
            "archive integrity enters the resolution"
        );
    }
    kaixa::PackageProvider* provider = registry.find_provider("local");
    context.check(provider != nullptr, "registry provider is configured");
    if (provider) {
        auto results = provider->query({"download"});
        context.check(results.has_value() && results->size() == 1, "registry catalog can be searched");
    }

    kaixa::ExtensionRegistry frozen_registry = kaixa::plugin::default_registry();
    auto frozen = kaixa::resolve_workspace(
        root.path() / "workspace",
        kaixa::ResolutionOptions{{}, &frozen_registry, root.path() / "cache", {}, nullptr, {}, kaixa::LockMode::frozen}
    );
    context.check(frozen.has_value(), "published package resolves from cache in frozen mode");
    if (!frozen)
        context.fail(kaixa::format_diagnostic(frozen.error()));
}

KAIXA_TEST(registry_provider_loads_its_index_on_first_use) {
    const TempDirectory root("lazy-registry-provider");
    kaixa::ExtensionRegistry registry = kaixa::plugin::default_registry();
    const kaixa::ProviderDefinition definition{
        "remote",
        "kaixa-registry",
        true,
        kaixa::Value::table({{"index", "missing.toml"}}),
        {},
    };
    const auto configured = registry.configure_provider(definition, {root.path()});
    context.check(configured.has_value(), "registry configuration does not read an unused index");
    if (!configured)
        return;

    kaixa::PackageProvider* provider = registry.find_provider("remote");
    context.check(provider != nullptr, "lazy registry provider is installed");
    if (!provider)
        return;

    const auto candidates = provider->candidates({"missing"});
    context.check(!candidates.has_value(), "first package request reads the unavailable index");
}

KAIXA_TEST(prebuilt_publication_materializes_an_opaque_package) {
    const TempDirectory root("published-prebuilt");
    root.write("library/Kaixa.toml", "[package]\nname = \"binary_library\"\nversion = \"2.0.0\"\nresolver = \"cmake\"\n");
    root.write("prebuilt/include/library.hpp", "#pragma once\n");
    auto published = kaixa::publish_package({root.path() / "library", root.path() / "registry", {}, {}, root.path() / "prebuilt"});
    context.check(published.has_value(), "prebuilt package publishes");
    if (!published)
        return;

    root.write(
        "workspace/Kaixa.toml",
        "[package]\nname = \"application\"\nresolver = \"cmake\"\n\n"
        "[dependencies]\nbinary_library = \"2\"\n\n"
        "[[provider]]\nname = \"local\"\ndriver = \"kaixa-registry\"\ndefault = true\nindex = \"../registry/index.toml\"\n"
    );
    kaixa::ExtensionRegistry registry = kaixa::plugin::default_registry();
    auto resolved = kaixa::resolve_workspace(
        root.path() / "workspace",
        kaixa::ResolutionOptions{{}, &registry, root.path() / "cache", {}, nullptr, {}, kaixa::LockMode::update}
    );
    context.check(resolved.has_value(), "prebuilt package resolves");
    if (!resolved) {
        context.fail(kaixa::format_diagnostic(resolved.error()));
        return;
    }
    const auto package = resolved->graph.find_by_name("binary_library");
    context.check(package.has_value(), "prebuilt package enters the graph");
    if (package) {
        context.check(resolved->graph[*package].is_opaque(), "prebuilt package stays opaque");
        context.check(
            std::filesystem::is_regular_file(resolved->graph[*package].directory / "include/library.hpp"),
            "artifact is extracted"
        );
    }
}

KAIXA_TEST(targeted_update_changes_only_an_explicitly_unlocked_candidate) {
    const TempDirectory root("targeted-update");
    root.write("library/Kaixa.toml", "[package]\nname = \"library\"\nversion = \"1.0.0\"\nresolver = \"cmake\"\n");
    auto first_publish = kaixa::publish_package({root.path() / "library", root.path() / "registry"});
    context.check(first_publish.has_value(), "first version publishes");
    if (!first_publish)
        return;

    root.write(
        "workspace/Kaixa.toml",
        "[package]\nname = \"application\"\nresolver = \"cmake\"\n\n"
        "[dependencies]\nlibrary = \"^1\"\n\n"
        "[[provider]]\nname = \"local\"\ndriver = \"kaixa-registry\"\ndefault = true\nindex = \"../registry/index.toml\"\n"
    );
    kaixa::ExtensionRegistry initial_registry = kaixa::plugin::default_registry();
    auto initial = kaixa::resolve_workspace(
        root.path() / "workspace",
        kaixa::ResolutionOptions{{}, &initial_registry, root.path() / "cache", {}, nullptr, {}, kaixa::LockMode::update}
    );
    context.check(initial.has_value(), "initial version resolves");
    if (!initial)
        return;

    root.write("library/Kaixa.toml", "[package]\nname = \"library\"\nversion = \"1.1.0\"\nresolver = \"cmake\"\n");
    auto second_publish = kaixa::publish_package({root.path() / "library", root.path() / "registry"});
    context.check(second_publish.has_value(), "new version publishes");
    if (!second_publish)
        return;

    kaixa::ExtensionRegistry pinned_registry = kaixa::plugin::default_registry();
    auto pinned = kaixa::resolve_workspace(
        root.path() / "workspace",
        kaixa::ResolutionOptions{{}, &pinned_registry, root.path() / "cache", {}, nullptr, {}, kaixa::LockMode::update}
    );
    context.check(pinned.has_value(), "ordinary resolution remains pinned");
    if (pinned) {
        const auto package = pinned->graph.find_by_name("library");
        context.check(
            package && pinned->graph[*package].source && pinned->graph[*package].source->version->text == "1.0.0",
            "lock retains version 1.0.0"
        );
    }

    const std::vector<std::string> unlocked{"library"};
    kaixa::ExtensionRegistry update_registry = kaixa::plugin::default_registry();
    auto updated = kaixa::resolve_workspace(
        root.path() / "workspace",
        kaixa::ResolutionOptions{{}, &update_registry, root.path() / "cache", {}, nullptr, {}, kaixa::LockMode::update, {}, unlocked}
    );
    context.check(updated.has_value(), "targeted update resolves");
    if (updated) {
        const auto package = updated->graph.find_by_name("library");
        context.check(
            package && updated->graph[*package].source && updated->graph[*package].source->version->text == "1.1.0",
            "unlocked package advances to version 1.1.0"
        );
    }
}

KAIXA_TEST(dependency_edits_preserve_unrelated_manifest_text) {
    const TempDirectory root("dependency-edit");
    root.write(
        "Kaixa.toml",
        "# retained comment\n[package]\nname = \"application\"\nresolver = \"cmake\"\n\n"
        "[dependencies]\nexisting = \"1\" # retained dependency comment\n\n[cmake]\nlanguages = [\"CXX\"]\n"
    );
    auto added = kaixa::add_manifest_dependency(root.path() / "Kaixa.toml", "new_package", "^2.1", "local");
    context.check(added.has_value(), "dependency addition is prepared");
    if (!added)
        return;

    context.check_contains(added->after, "# retained comment", "top-level comment is preserved");
    context.check_contains(added->after, "existing = \"1\" # retained", "dependency comment is preserved");
    context.check_contains(added->after, "new_package = { version = \"^2.1\", from = \"local\" }", "new binding is formatted");
    auto removed = kaixa::remove_manifest_dependency(root.path() / "Kaixa.toml", "existing");
    context.check(removed.has_value(), "dependency removal is prepared from the original document");
    if (removed)
        context.check(!removed->after.contains("existing ="), "only selected dependency is removed");
}

KAIXA_TEST(dependency_edits_match_dotted_names_literally) {
    const TempDirectory root("dependency-edit-dotted-name");
    root.write(
        "Kaixa.toml",
        "[package]\nname = \"application\"\nresolver = \"cmake\"\n\n"
        "[dependencies]\nfooXbar = \"1\"\nfoo.bar = \"2\"\n"
    );

    auto removed = kaixa::remove_manifest_dependency(root.path() / "Kaixa.toml", "foo.bar");
    context.check(removed.has_value(), "dotted dependency removal is prepared");
    if (!removed)
        return;

    context.check_contains(removed->after, "fooXbar = \"1\"", "regex-like sibling name is preserved");
    context.check(!removed->after.contains("foo.bar ="), "exact dotted dependency is removed");
}

KAIXA_TEST(publication_rejects_machine_local_dependencies) {
    const TempDirectory root("publication-local-dependency");
    root.write(
        "library/Kaixa.toml",
        "[package]\nname = \"library\"\nversion = \"1.0.0\"\nresolver = \"cmake\"\n\n"
        "[dependencies]\nprivate = { path = \"../private\" }\n"
    );
    auto published = kaixa::publish_package({root.path() / "library", root.path() / "registry"});
    context.check(!published.has_value(), "package with a path dependency is not published");
    if (!published)
        context.check_contains(published.error().message, "local path", "publication diagnostic identifies the dependency");
}
