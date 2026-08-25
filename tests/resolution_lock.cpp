#include <test_support.hpp>

#include <kaixa/kaixa.hpp>
#include <kaixa/plugin/bundle.hpp>

#include <filesystem>
#include <string>
#include <string_view>

using kaixa::testing::TempDirectory;

namespace {
    std::string package_map_manifest(const bool include_locked, const bool include_newer) {
        std::string packages;
        if (include_locked) {
            packages += R"({ name = "component", version = "1.0.0", consumer = { resolver = "cmake" } })";
        }
        if (include_locked && include_newer)
            packages += ", ";

        if (include_newer) {
            packages += R"({ name = "component", version = "1.5.0", consumer = { resolver = "cmake" } })";
        }

        return R"([package]
name = "app"
version = "1.0.0"
resolver = "cmake"

[dependencies]
component = "1"

[providers.official]
driver = "package-map"
default = true
package = [)"
            + packages
            + "]\n";
    }

    std::string routed_package_map_manifest(const std::string_view default_provider) {
        const auto provider = [&](const std::string_view name) {
            return "\n[providers."
                + std::string(name)
                + "]\ndriver = \"package-map\"\ndefault = "
                + (name == default_provider ? "true" : "false")
                + "\npackage = [{ name = \"component\", version = \"1.0.0\", consumer = { resolver = \"cmake\" } }]\n";
        };
        return "[package]\nname = \"app\"\nresolver = \"cmake\"\n\n[dependencies]\ncomponent = \"1\"\n"
            + provider("official")
            + provider("company");
    }

    std::string explicitly_routed_package_map_manifest(const std::string_view selected_provider) {
        return "[package]\n"
               "name = \"app\"\n"
               "resolver = \"cmake\"\n"
               "\n"
               "[dependencies]\n"
               "component = \"1\"\n"
               "\n"
               "[routing]\n"
               "component = \""
            + std::string(selected_provider)
            + "\"\n"
              "\n"
              "[providers.official]\n"
              "driver = \"package-map\"\n"
              "package = [{ name = \"component\", version = \"1.0.0\", consumer = { resolver = \"cmake\" } }]\n"
              "\n"
              "[providers.company]\n"
              "driver = \"package-map\"\n"
              "package = [{ name = \"component\", version = \"1.0.0\", consumer = { resolver = \"cmake\" } }]\n";
    }

    kaixa::Result<kaixa::PackageResolution> resolve_locked_workspace(const TempDirectory& root, const kaixa::LockMode mode) {
        kaixa::ExtensionRegistry extensions = kaixa::plugin::default_registry();
        return kaixa::resolve_workspace(
            root.path(),
            kaixa::ResolutionOptions{{}, &extensions, {}, {}, nullptr, kaixa::PolicyContext{"debug", "windows"}, mode}
        );
    }
}

KAIXA_TEST(resolution_lock_routes_through_the_pinned_provider) {
    const TempDirectory root("resolution-lock-provider-route");
    root.write("Kaixa.toml", routed_package_map_manifest("official"));

    const auto initial = resolve_locked_workspace(root, kaixa::LockMode::update);
    context.check(initial.has_value(), "initial default provider resolves");
    if (!initial)
        return;

    root.write("Kaixa.toml", routed_package_map_manifest("company"));
    const auto pinned = resolve_locked_workspace(root, kaixa::LockMode::update);
    context.check(pinned.has_value(), "lock routes independently of the new default provider");
    if (!pinned)
        return;

    const auto component = pinned->graph.find_by_name("component");
    context.check(component.has_value(), "routed package enters the graph");
    if (!component || !pinned->graph[*component].source)
        return;

    context.check_equal(
        pinned->graph[*component].source->provider.value_or(""),
        std::string("official"),
        "locked provider remains authoritative"
    );
}

KAIXA_TEST(targeted_update_may_change_the_locked_provider_route) {
    const TempDirectory root("resolution-lock-provider-update");
    root.write("Kaixa.toml", explicitly_routed_package_map_manifest("official"));

    const auto initial = resolve_locked_workspace(root, kaixa::LockMode::update);
    context.check(initial.has_value(), "initial explicit provider resolves");
    if (!initial)
        return;

    root.write("Kaixa.toml", explicitly_routed_package_map_manifest("company"));
    kaixa::ExtensionRegistry extensions = kaixa::plugin::default_registry();
    const std::vector<std::string> unlocked{"component"};
    const auto updated = kaixa::resolve_workspace(
        root.path(),
        kaixa::ResolutionOptions{{},
            &extensions,
            {},
            {},
            nullptr,
            kaixa::PolicyContext{"debug", "windows"},
            kaixa::LockMode::update,
            {},
            unlocked}
    );
    context.check(updated.has_value(), "targeted update follows the new provider route");
    if (!updated)
        return;

    const auto component = updated->graph.find_by_name("component");
    context.check(component.has_value(), "updated package enters the graph");
    if (component && updated->graph[*component].source) {
        context.check_equal(
            updated->graph[*component].source->provider.value_or(""),
            std::string("company"),
            "unlocked package is no longer pinned to the previous provider"
        );
    }
}

KAIXA_TEST(resolution_lock_pins_provider_candidate_and_updates_when_unavailable) {
    const TempDirectory root("resolution-lock-provider-pin");
    root.write("Kaixa.toml", package_map_manifest(true, false));

    const auto initial = resolve_locked_workspace(root, kaixa::LockMode::update);
    context.check(initial.has_value(), "initial resolution creates the lock");
    if (!initial) {
        context.fail(kaixa::format_diagnostic(initial.error()));
        return;
    }
    context.check(initial->lock_changed, "initial lock is reported as changed");
    context.check(std::filesystem::is_regular_file(root.path() / "Kaixa.lock"), "Kaixa.lock is written at the context root");

    root.write("Kaixa.toml", package_map_manifest(true, true));
    const auto pinned = resolve_locked_workspace(root, kaixa::LockMode::update);
    context.check(pinned.has_value(), "existing lock remains usable when a newer version appears");
    if (!pinned)
        return;

    const auto component_id = pinned->graph.find_by_name("component");
    context.check(component_id.has_value(), "locked provider package enters the graph");
    if (component_id && pinned->graph[*component_id].source && pinned->graph[*component_id].source->version) {
        context.check_equal(
            pinned->graph[*component_id].source->version->text,
            std::string("1.0.0"),
            "locked version wins over a newer candidate"
        );
    } else {
        context.fail("opaque provider version is retained");
    }
    context.check(!pinned->lock_changed, "unchanged resolution does not rewrite the lock");

    root.write("Kaixa.toml", package_map_manifest(false, true));
    const auto strict = resolve_locked_workspace(root, kaixa::LockMode::locked);
    context.check(!strict.has_value(), "locked mode rejects a missing pinned candidate");
    if (!strict)
        context.check_contains(strict.error().message, "no longer offers the candidate", "missing pin diagnostic");

    const auto updated = resolve_locked_workspace(root, kaixa::LockMode::update);
    context.check(updated.has_value(), "update mode selects a replacement when the pin disappeared");
    if (!updated)
        return;

    const auto updated_component = updated->graph.find_by_name("component");
    context.check(updated_component.has_value(), "replacement package enters the graph");
    if (updated_component && updated->graph[*updated_component].source && updated->graph[*updated_component].source->version) {
        context
            .check_equal(updated->graph[*updated_component].source->version->text, std::string("1.5.0"), "replacement version is selected");
    }
    context.check(updated->lock_changed, "replacement updates Kaixa.lock");
}

KAIXA_TEST(resolution_lock_round_trips_sources_and_context_variants) {
    const TempDirectory root("resolution-lock-round-trip");
    root.write(
        "Kaixa.toml",
        R"([package]
name = "app"
version = "1.0.0"
resolver = "cmake"

[dependencies]
component = { path = "component" }
)"
    );
    root.write(
        "component/Kaixa.toml",
        R"([package]
name = "component"
version = "2.0.0"
resolver = "cmake"
)"
    );

    const auto resolved = resolve_locked_workspace(root, kaixa::LockMode::update);
    context.check(resolved.has_value(), "path resolution writes a lock");
    if (!resolved) {
        context.fail(kaixa::format_diagnostic(resolved.error()));
        return;
    }

    context.check_equal(resolved->context.manifest, root.path() / "Kaixa.toml", "resolution context keeps its root manifest");
    context.check_equal(resolved->context.lockfile, root.path() / "Kaixa.lock", "resolution context exposes its lockfile");
    context.check_equal(resolved->context.roots.front(), std::string("app"), "resolution context exposes selected roots");

    const auto lock = kaixa::read_resolution_lock(root.path() / "Kaixa.lock");
    context.check(lock.has_value() && lock->has_value(), "written lock parses");
    if (!lock || !*lock)
        return;

    const kaixa::LockedPackage* component = (**lock).find("component");
    context.check(component != nullptr, "path dependency is represented in the lock");
    if (!component)
        return;

    context.check_equal(component->version.value_or(""), std::string("2.0.0"), "manifest version is locked");
    context.check_equal(component->authority.value_or(""), std::string("direct"), "direct authority is locked");
    context.check_equal(component->source_driver.value_or(""), std::string("path"), "source driver is locked");
    context.check(!component->resolutions.empty(), "resolution context is locked");
    if (!component->resolutions.empty()) {
        context.check_equal(component->resolutions.front().profile, std::string("debug"), "profile is locked");
        context.check_equal(component->resolutions.front().target, std::string("windows"), "target is locked");
        context.check(!component->resolutions.front().variants.empty(), "configured variants are locked");
    }

    const auto strict = resolve_locked_workspace(root, kaixa::LockMode::frozen);
    context.check(strict.has_value(), "frozen mode accepts the identical local resolution");
    if (!strict)
        context.fail(kaixa::format_diagnostic(strict.error()));
}

KAIXA_TEST(locked_mode_rejects_changed_features_and_variants) {
    const TempDirectory root("resolution-lock-stale-variant");
    root.write("Kaixa.toml", "[package]\nname = \"app\"\nversion = \"1.0.0\"\nresolver = \"cmake\"\n");

    const auto initial = resolve_locked_workspace(root, kaixa::LockMode::update);
    context.check(initial.has_value(), "initial local resolution is locked");
    if (!initial)
        return;

    root.write(
        "Kaixa.toml",
        "[package]\n"
        "name = \"app\"\n"
        "version = \"1.0.0\"\n"
        "resolver = \"cmake\"\n"
        "\n"
        "[features]\n"
        "default = [\"instrumented\"]\n"
        "instrumented = []\n"
    );
    const auto strict = resolve_locked_workspace(root, kaixa::LockMode::locked);
    context.check(!strict.has_value(), "locked mode rejects changed effective features");
    if (!strict)
        context.check_contains(strict.error().message, "is stale", "stale resolution diagnostic");
}

KAIXA_TEST(resolution_lock_prunes_packages_removed_from_a_context) {
    const TempDirectory root("resolution-lock-prune");
    root.write("Kaixa.toml", "[package]\nname = \"app\"\nresolver = \"cmake\"\n\n[dependencies]\ncomponent = { path = \"component\" }\n");
    root.write("component/Kaixa.toml", "[package]\nname = \"component\"\nversion = \"1.0.0\"\nresolver = \"cmake\"\n");

    const auto initial = resolve_locked_workspace(root, kaixa::LockMode::update);
    context.check(initial.has_value(), "dependency is initially locked");
    if (!initial)
        return;

    root.write("Kaixa.toml", "[package]\nname = \"app\"\nresolver = \"cmake\"\n");
    const auto updated = resolve_locked_workspace(root, kaixa::LockMode::update);
    context.check(updated.has_value(), "context updates after removing a dependency");
    if (!updated)
        return;

    const auto lock = kaixa::read_resolution_lock(root.path() / "Kaixa.lock");
    context.check(lock.has_value() && lock->has_value(), "updated lock parses");
    if (lock && *lock)
        context.check((**lock).find("component") == nullptr, "unreferenced package is pruned from this context");
}

KAIXA_TEST(locked_mode_requires_an_existing_lockfile) {
    const TempDirectory root("resolution-lock-required");
    root.write("Kaixa.toml", "[package]\nname = \"app\"\nresolver = \"cmake\"\n");

    const auto resolved = resolve_locked_workspace(root, kaixa::LockMode::locked);
    context.check(!resolved.has_value(), "locked mode does not create a missing lock");
    if (!resolved)
        context.check_contains(resolved.error().message, "lockfile does not exist", "missing lock diagnostic");
}
