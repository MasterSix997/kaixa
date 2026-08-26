#pragma once

#include <kaixa/model/manifest.hpp>
#include <kaixa/workspace/package_index.hpp>

#include <filesystem>
#include <optional>
#include <string_view>

namespace kaixa::workspace_detail {
    struct PreparedManagedPackage {
        std::filesystem::path manifest_path;
        Manifest manifest;
        std::vector<PackageTarget> targets;
    };

    [[nodiscard]] Result<PreparedManagedPackage> prepare_managed_package(
        PackageIndex& packages,
        const std::filesystem::path& directory,
        std::optional<std::string_view> expected_name,
        const SourceLocation& declaration,
        const ManifestDocument* parsed_document = nullptr
    );
}
