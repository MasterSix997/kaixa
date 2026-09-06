#include <test_support.hpp>

#include <workspace/package_routes.hpp>

#include <kaixa/model/graph.hpp>
#include <kaixa/model/manifest.hpp>
#include <kaixa/workspace/package_index.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using kaixa::testing::TempDirectory;

namespace {
    // A route is driven directly here: it receives only a graph, a package index, the two
    // contexts and the re-entry callback. Nothing else of the workspace loader is reachable.
    struct RouteHarness {
        kaixa::Graph graph;
        std::optional<kaixa::PackageIndex> packages;
        std::map<std::string, std::string> routing;
        std::vector<std::filesystem::path> managed_requests;

        kaixa::Result<void> open(const std::filesystem::path& manifest) {
            auto document = kaixa::parse_manifest_document_file(manifest);
            if (!document)
                return std::unexpected(document.error());

            m_document = std::move(*document);
            auto index = kaixa::PackageIndex::discover(manifest, *m_document);
            if (!index)
                return std::unexpected(index.error());

            packages = std::move(*index);
            m_manifest = manifest;
            return {};
        }

        kaixa::workspace_detail::PackageRouteContext context() {
            return {graph,
                *packages,
                kaixa::workspace_detail::SourceMaterializationContext{},
                kaixa::workspace_detail::DependencyRoutingContext{nullptr,
                    *packages,
                    nullptr,
                    routing,
                    kaixa::LockMode::none,
                    m_manifest.parent_path(),
                    {},
                    false},
                [this](const std::filesystem::path& manifest, const std::optional<std::string>&, const kaixa::SourceLocation&)
                    -> kaixa::Result<kaixa::PackageId> {
                    managed_requests.push_back(manifest);
                    kaixa::PackageNode node;
                    node.name = "vendor";
                    node.directory = manifest.parent_path();
                    node.resolver = "cmake";
                    node.semantics = kaixa::ManagedPackage{kaixa::Manifest{"vendor", "cmake"}};
                    return graph.add(std::move(node));
                }};
        }

    private:
        std::optional<kaixa::ManifestDocument> m_document;
        std::filesystem::path m_manifest;
    };

    kaixa::DependencyBinding path_dependency(std::string name, std::filesystem::path relative) {
        kaixa::DependencyBinding dependency(std::move(name), std::move(relative));
        return dependency;
    }
}

KAIXA_TEST(path_route_adopts_a_directory_without_a_manifest_on_its_own) {
    const TempDirectory root("path-route-opaque");
    root.write("Kaixa.toml", "[package]\nname = \"app\"\nresolver = \"cmake\"\n");
    root.write("vendor/data.txt", "payload\n");

    RouteHarness harness;
    const auto opened = harness.open(root.path() / "Kaixa.toml");
    context.check(opened.has_value(), "harness opens the workspace");
    if (!opened) {
        context.fail(kaixa::format_diagnostic(opened.error()));
        return;
    }

    const kaixa::DependencyBinding dependency = path_dependency("vendor", "vendor");
    auto route_context = harness.context();
    const auto loaded = kaixa::workspace_detail::load_path_route(route_context, root.path(), dependency);
    context.check(loaded.has_value(), "the path route resolves a manifest-free directory");
    if (!loaded) {
        context.fail(kaixa::format_diagnostic(loaded.error()));
        return;
    }

    const kaixa::PackageNode& node = harness.graph[*loaded];
    context.check(node.is_opaque(), "a manifest-free path becomes an opaque package");
    context.check_equal(node.name, std::string("vendor"), "the dependency name becomes the package name");
    context.check(node.source.has_value() && node.source->authority == "direct", "the route records direct provenance");
    context.check(harness.managed_requests.empty(), "the route does not load a managed package itself");
}

KAIXA_TEST(path_route_hands_a_managed_directory_back_to_its_caller) {
    const TempDirectory root("path-route-managed");
    root.write("Kaixa.toml", "[package]\nname = \"app\"\nresolver = \"cmake\"\n");
    root.write("vendor/Kaixa.toml", "[package]\nname = \"vendor\"\nversion = \"1.0.0\"\nresolver = \"cmake\"\n");

    RouteHarness harness;
    const auto opened = harness.open(root.path() / "Kaixa.toml");
    context.check(opened.has_value(), "harness opens the workspace");
    if (!opened)
        return;

    const kaixa::DependencyBinding dependency = path_dependency("vendor", "vendor");
    auto route_context = harness.context();
    const auto loaded = kaixa::workspace_detail::load_path_route(route_context, root.path(), dependency);
    context.check(loaded.has_value(), "the path route resolves a managed directory");
    if (!loaded) {
        context.fail(kaixa::format_diagnostic(loaded.error()));
        return;
    }

    context.check_equal(harness.managed_requests.size(), std::size_t{1}, "managed loading is delegated exactly once");
    if (!harness.managed_requests.empty()) {
        context.check_equal(
            harness.managed_requests.front().filename().generic_string(),
            std::string("Kaixa.toml"),
            "the route hands back the manifest it found"
        );
    }
    context.check(harness.graph[*loaded].is_managed(), "the delegated package keeps managed semantics");
    context.check(harness.graph[*loaded].source.has_value(), "the route stamps provenance on the delegated package");
}

KAIXA_TEST(path_route_rejects_a_version_requirement_it_cannot_satisfy) {
    const TempDirectory root("path-route-version");
    root.write("Kaixa.toml", "[package]\nname = \"app\"\nresolver = \"cmake\"\n");
    root.write("vendor/data.txt", "payload\n");

    RouteHarness harness;
    const auto opened = harness.open(root.path() / "Kaixa.toml");
    context.check(opened.has_value(), "harness opens the workspace");
    if (!opened)
        return;

    kaixa::DependencyBinding dependency = path_dependency("vendor", "vendor");
    dependency.request.version = kaixa::VersionRequirement{"1.0.0"};
    auto route_context = harness.context();
    const auto loaded = kaixa::workspace_detail::load_path_route(route_context, root.path(), dependency);
    context.check(!loaded.has_value(), "an opaque path cannot satisfy a version requirement");
    if (!loaded)
        context.check_contains(loaded.error().message, "cannot satisfy a version requirement", "diagnostic names the conflict");
}
