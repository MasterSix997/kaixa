#pragma once

#include "dependency_routing.hpp"
#include "source_materialization.hpp"

#include <kaixa/model/package.hpp>
#include <kaixa/workspace/package_index.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace kaixa {
    class Graph;
}

namespace kaixa::workspace_detail {
    // Re-entry into managed package loading. A route resolves a dependency down to a manifest and
    // hands it back; it never loads a managed package itself.
    using LoadManagedPackage = std::
        function<Result<PackageId>(const std::filesystem::path& manifest, std::optional<std::string> name, const SourceLocation& location)>;

    // Every service a dependency route may touch, and nothing else. A route cannot reach the
    // workspace loader's lockfile, feature settings or manifest model.
    struct PackageRouteContext {
        Graph& graph;
        PackageIndex& packages;
        SourceMaterializationContext sources;
        DependencyRoutingContext routing;
        LoadManagedPackage load_managed;
    };

    [[nodiscard]] Result<PackageId> load_path_route(
        const PackageRouteContext& context,
        const std::filesystem::path& requester,
        const DependencyBinding& dependency
    );

    [[nodiscard]] Result<PackageId> load_source_route(
        const PackageRouteContext& context,
        const SourceLocator& source,
        const std::filesystem::path& requester,
        const DependencyBinding& dependency,
        std::optional<std::string> provider,
        std::string authority,
        const std::optional<Version>& expected_version = std::nullopt
    );

    [[nodiscard]] Result<PackageId> load_provider_route(
        const PackageRouteContext& context,
        const PackageProvider& provider,
        const std::filesystem::path& requester,
        const DependencyBinding& dependency
    );
}
