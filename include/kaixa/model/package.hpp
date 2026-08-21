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

    enum class PackageKind { managed, opaque };

    struct PackageTargetDependencies {
        std::string target;
        PackageTargetKind kind = PackageTargetKind::test;
        std::vector<PackageId> packages;
    };

    struct PackageSource {
        std::optional<std::string> provider;
        std::string authority;
        SourceLocator locator;
        std::optional<std::string> identity;
    };

    struct PackageNode {
        PackageId id;
        std::string name;
        std::filesystem::path directory;
        PackageKind kind = PackageKind::opaque;
        std::string resolver;
        std::optional<Manifest> manifest;
        std::vector<PackageId> dependencies;
        std::vector<PackageTargetDependencies> target_dependencies;
        std::optional<PackageSource> source;
        std::optional<Value> descriptor;
        std::vector<std::string> active_features;
        std::vector<Value> policy_layers;
    };

    struct ConfiguredPackageInstance {
        PackageId package;
        std::string artifact;
        std::vector<std::string> features;
        std::vector<Value> policy_layers;
    };
}
