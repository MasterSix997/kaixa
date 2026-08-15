#pragma once

#include <kaixa/extension/registry.hpp>
#include <kaixa/foundation/diagnostic.hpp>
#include <kaixa/model/graph.hpp>
#include <kaixa/workspace/package_index.hpp>

#include <filesystem>
#include <span>
#include <string>

namespace kaixa {
    struct PackageResolution {
        Graph graph;
        PackageIndex available;
        ConfigurationSet configurations;
        std::filesystem::path manifest;
    };

    struct ResolutionOptions {
        std::span<const std::string> packages;
        const ExtensionRegistry* extensions = nullptr;
        std::filesystem::path source_cache;
    };

    [[nodiscard]] Result<std::filesystem::path> find_manifest(const std::filesystem::path& start);
    [[nodiscard]] Result<PackageResolution> resolve_workspace(const std::filesystem::path& start, const ResolutionOptions& options);
    [[nodiscard]] Result<PackageResolution> resolve_workspace(const std::filesystem::path& start, std::span<const std::string> selected_packages = {});
    [[nodiscard]] Result<Graph> load_workspace(const std::filesystem::path& start);
}
