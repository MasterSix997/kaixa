#pragma once

#include <kaixa/model/manifest.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace kaixa {
    struct PackageId {
        std::size_t index = 0;

        [[nodiscard]] bool operator==(const PackageId&) const = default;
    };

    enum class PackageKind {
        managed,
        opaque
    };

    struct PackageNode {
        PackageId id;
        std::string name;
        std::filesystem::path directory;
        PackageKind kind = PackageKind::opaque;
        std::string resolver;
        std::optional<Manifest> manifest;
        std::vector<PackageId> dependencies;
    };
}
