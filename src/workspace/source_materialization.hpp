#pragma once

#include <kaixa/extension/registry.hpp>
#include <kaixa/workspace/resolution_lock.hpp>

#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace kaixa::workspace_detail {
    enum class MaterializationKind {
        package_source,
        prebuilt_artifact
    };

    struct SourceMaterializationContext {
        ExtensionRegistry* extensions;
        std::filesystem::path cache;
        LockMode lock_mode;
        const ResolutionLock* lock;
        std::span<const std::string> unlocked_packages;
        bool unlock_all;
        bool refresh;
        std::function<void(std::string_view)> progress;
    };

    [[nodiscard]] Result<std::filesystem::path> canonical_directory(const std::filesystem::path& path, const SourceLocation& location = {});
    [[nodiscard]] Result<SourceTree> materialize_source(
        const SourceMaterializationContext& context,
        const SourceLocator& source,
        const std::filesystem::path& requester,
        std::string_view package,
        const SourceLocation& location,
        MaterializationKind kind
    );
}
