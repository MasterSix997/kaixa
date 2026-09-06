#pragma once

#include <kaixa/model/manifest.hpp>
#include <kaixa/model/policy.hpp>

#include <compare>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace kaixa {
    class Graph;

    struct PackageId {
        std::size_t index = 0;

        [[nodiscard]] bool operator==(const PackageId&) const = default;
        [[nodiscard]] auto operator<=>(const PackageId&) const = default;
    };

    // A managed package is described by a Kaixa manifest. An adopted package wraps a foreign
    // project: the loader synthesizes a manifest for it and keeps the provider descriptor that
    // says how to consume it. An opaque package has no build semantics at all - only, sometimes,
    // a descriptor. Encoding this as alternatives makes a managed package without a manifest, and
    // an opaque package with one, unrepresentable.
    struct ManagedPackage {
        Manifest manifest;
    };

    struct AdoptedPackage {
        Manifest manifest;
        Value descriptor;
    };

    struct OpaquePackage {
        std::optional<Value> descriptor;
    };

    using PackageSemantics = std::variant<ManagedPackage, AdoptedPackage, OpaquePackage>;

    struct PackageTargetDependencies {
        std::string target;
        PackageTargetKind kind = PackageTargetKind::test;
        std::vector<PackageId> packages;
    };

    struct PackageSource {
        std::optional<std::string> provider;
        std::string authority;
        std::optional<Version> version;
        std::optional<SourceLocator> locator;
        std::optional<std::string> identity;
        std::optional<std::string> integrity;
    };

    struct PackageNode {
        PackageId id;
        std::string name;
        std::filesystem::path directory;
        std::string resolver;
        PackageSemantics semantics = OpaquePackage{};
        std::vector<PackageId> dependencies;
        std::vector<PackageTargetDependencies> target_dependencies;
        std::optional<PackageSource> source;
        std::vector<std::string> active_features;
        std::vector<Value> policy_layers;
        std::vector<PackageTarget> targets;

        [[nodiscard]] const Manifest* manifest() const noexcept {
            if (const auto* managed = std::get_if<ManagedPackage>(&semantics))
                return &managed->manifest;

            if (const auto* adopted = std::get_if<AdoptedPackage>(&semantics))
                return &adopted->manifest;

            return nullptr;
        }

        [[nodiscard]] Manifest* manifest() noexcept {
            if (auto* managed = std::get_if<ManagedPackage>(&semantics))
                return &managed->manifest;

            if (auto* adopted = std::get_if<AdoptedPackage>(&semantics))
                return &adopted->manifest;

            return nullptr;
        }

        [[nodiscard]] const Value* descriptor() const noexcept {
            if (const auto* adopted = std::get_if<AdoptedPackage>(&semantics))
                return &adopted->descriptor;

            if (const auto* opaque = std::get_if<OpaquePackage>(&semantics))
                return opaque->descriptor ? &*opaque->descriptor : nullptr;

            return nullptr;
        }

        [[nodiscard]] bool has_build_semantics() const noexcept { return !std::holds_alternative<OpaquePackage>(semantics); }

        [[nodiscard]] bool is_managed() const noexcept { return std::holds_alternative<ManagedPackage>(semantics); }
        [[nodiscard]] bool is_adopted() const noexcept { return std::holds_alternative<AdoptedPackage>(semantics); }
        [[nodiscard]] bool is_opaque() const noexcept { return std::holds_alternative<OpaquePackage>(semantics); }
    };

    struct ConfiguredPackageInstance {
        PackageId package;
        std::string artifact;
        std::vector<std::string> features;
        std::vector<Value> policy_layers;
        EffectivePolicy policy;
        std::vector<std::string> contexts;
    };

    [[nodiscard]] Result<std::vector<ConfiguredPackageInstance>> configure_package_instances(
        const Graph& graph,
        const PolicyContext& context,
        const PolicySchema& schema
    );
    [[nodiscard]] const ConfiguredPackageInstance* find_configured_package_instance(
        std::span<const ConfiguredPackageInstance> instances,
        PackageId package,
        std::string_view context
    ) noexcept;
}
