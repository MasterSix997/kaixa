#pragma once

#include <kaixa/extension/registry.hpp>
#include <kaixa/foundation/diagnostic.hpp>
#include <kaixa/model/graph.hpp>
#include <kaixa/workspace/package_index.hpp>
#include <kaixa/workspace/resolution_lock.hpp>

#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace kaixa {
    struct ResolutionContext {
        std::filesystem::path manifest;
        std::filesystem::path directory;
        std::filesystem::path lockfile;
        std::vector<std::string> roots;
        PolicyContext policy;
        LockMode lock_mode = LockMode::none;
    };

    struct PackageResolution {
        Graph graph;
        PackageIndex available;
        ConfigurationSet configurations;
        std::filesystem::path manifest;
        ResolutionContext context;
        bool lock_changed = false;
        ManifestTree model;
        std::vector<ConfiguredPackageInstance> instances;
    };

    struct ResolutionOptions {
        std::span<const std::string> packages;
        ExtensionRegistry* extensions = nullptr;
        std::filesystem::path source_cache;
        std::span<const ProviderLayer> provider_layers;
        const Value* feature_settings = nullptr;
        PolicyContext policy_context;
        LockMode lock_mode = LockMode::none;
        std::filesystem::path lockfile;
        std::span<const std::string> unlocked_packages;
        bool unlock_all = false;
        bool write_lock = true;
        bool refresh_sources = true;
        std::function<void(std::string_view)> source_progress;
        bool load_model = true;
    };

    [[nodiscard]] Result<std::filesystem::path> find_manifest(const std::filesystem::path& start);
    [[nodiscard]] Result<PackageResolution> resolve_workspace(const std::filesystem::path& start, const ResolutionOptions& options);
    [[nodiscard]] Result<PackageResolution> resolve_workspace(
        const std::filesystem::path& manifest,
        const ManifestDocument& document,
        const ResolutionOptions& options
    );
    [[nodiscard]] Result<PackageResolution> resolve_workspace(
        const std::filesystem::path& start,
        std::span<const std::string> selected_packages = {}
    );
    [[nodiscard]] Result<Graph> load_workspace(const std::filesystem::path& start);
}
