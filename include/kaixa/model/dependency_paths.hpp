#pragma once

#include <kaixa/model/graph.hpp>

#include <filesystem>
#include <string_view>

namespace kaixa {
    // Resolving a dependency by the local name a manifest gave it, and locating its materialized
    // source tree, are graph mechanisms. They carry no language or resolver semantics, so an
    // extension interpreting its own product options can reuse them instead of walking the graph.
    [[nodiscard]] Result<const PackageNode*> find_dependency_by_local_name(
        const Graph& graph,
        const PackageNode& package,
        std::string_view local_name,
        const SourceLocation& location
    );
    [[nodiscard]] Result<std::filesystem::path> dependency_source_directory(const PackageNode& dependency, const SourceLocation& location);
}
