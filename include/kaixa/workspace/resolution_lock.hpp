#pragma once

#include <kaixa/extension/provider.hpp>
#include <kaixa/model/graph.hpp>

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kaixa {
    enum class LockMode {
        none,
        update,
        locked,
        frozen
    };

    struct LockedVariant {
        std::vector<std::string> features;
        std::string policy;
        std::vector<std::string> contexts;

        [[nodiscard]] bool operator==(const LockedVariant&) const = default;
    };

    struct LockedPackageResolution {
        std::vector<std::string> roots;
        std::string profile;
        std::string target;
        std::vector<std::string> features;
        std::vector<std::string> dependencies;
        std::vector<LockedVariant> variants;

        [[nodiscard]] bool operator==(const LockedPackageResolution&) const = default;
    };

    struct LockedPackage {
        std::string name;
        std::optional<std::string> version;
        std::string resolver;
        std::optional<std::string> provider;
        std::optional<std::string> authority;
        std::optional<std::string> source_driver;
        std::optional<Value> source_options;
        std::optional<std::string> source_identity;
        std::optional<std::string> source_integrity;
        std::vector<LockedPackageResolution> resolutions;
    };

    struct ResolutionLock {
        std::vector<LockedPackage> packages;

        [[nodiscard]] const LockedPackage* find(std::string_view package) const noexcept;
    };

    [[nodiscard]] Result<std::optional<ResolutionLock>> read_resolution_lock(const std::filesystem::path& path);
    [[nodiscard]] Result<std::string> format_resolution_lock(const ResolutionLock& lock);
    [[nodiscard]] Result<bool> write_resolution_lock(const std::filesystem::path& path, const ResolutionLock& lock);
    [[nodiscard]] ResolutionLock capture_resolution_lock(
        const Graph& graph,
        std::span<const ConfiguredPackageInstance> instances,
        const PolicyContext& context,
        const std::filesystem::path& context_directory
    );
    [[nodiscard]] ResolutionLock merge_resolution_lock(ResolutionLock existing, const ResolutionLock& current);
    [[nodiscard]] bool resolution_locks_equal(const ResolutionLock& left, const ResolutionLock& right);
    [[nodiscard]] Result<void> validate_resolution_lock(const ResolutionLock& expected, const ResolutionLock& current);
    [[nodiscard]] bool locked_candidate_matches(
        const LockedPackage& locked,
        std::string_view provider,
        const PackageCandidate& candidate,
        const std::filesystem::path& context_directory
    );
}
