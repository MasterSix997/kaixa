#pragma once

#include <kaixa/extension/registry.hpp>
#include <kaixa/workspace/package_index.hpp>
#include <kaixa/workspace/resolution_lock.hpp>

#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <variant>

namespace kaixa::workspace_detail {
    struct PathDependencyRoute {};

    struct SourceDependencyRoute {};

    struct ProviderDependencyRoute {
        PackageProvider& provider;
    };

    struct LocalDependencyRoute {
        LocalPackageCandidate candidate;
    };

    using DependencyRoute = std::variant<PathDependencyRoute, SourceDependencyRoute, ProviderDependencyRoute, LocalDependencyRoute>;

    struct DependencyRoutingContext {
        ExtensionRegistry* extensions;
        const PackageIndex& packages;
        const ResolutionLock* lock;
        const std::map<std::string, std::string>& routing;
        LockMode lock_mode;
        std::filesystem::path context_directory;
        std::span<const std::string> unlocked_packages;
        bool unlock_all;
    };

    [[nodiscard]] Result<DependencyRoute> select_dependency_route(
        const DependencyRoutingContext& context,
        const std::filesystem::path& requester_manifest,
        const DependencyBinding& dependency
    );
    [[nodiscard]] Result<PackageCandidate> select_provider_candidate(
        const DependencyRoutingContext& context,
        const PackageProvider& provider,
        const DependencyBinding& dependency
    );
}
