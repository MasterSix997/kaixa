#pragma once

#include <kaixa/foundation/diagnostic.hpp>
#include <kaixa/model/manifest.hpp>

#include <filesystem>

namespace kaixa::workspace_detail {
    [[nodiscard]] Result<std::vector<PackageTarget>> normalize_package_targets(
        const Manifest& manifest,
        const std::filesystem::path& package_directory
    );
}
