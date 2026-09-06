#pragma once

#include <kaixa/model/graph.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace kaixa {
    enum class EffectiveProductType {
        executable,
        static_library,
        shared_library,
        interface_library
    };

    // Kaixa owns the artifact: its identity, kind, sources, runtime files and policy. Everything
    // that only a toolchain can interpret - headers, include paths, definitions, system libraries -
    // stays in `resolver_options` and is converted once by the resolver that implements it.
    struct EffectiveProduct {
        std::string name;
        EffectiveProductType type = EffectiveProductType::static_library;
        FileSet sources;
        FileSet runtime_files;
        std::vector<Value> policy_layers;
        Value resolver_options;
        SourceLocation location;
    };

    enum class TargetAvailability {
        available,
        skipped
    };

    struct EffectiveTarget {
        PackageTarget target;
        TargetAvailability availability = TargetAvailability::available;
        std::vector<std::string> skip_reasons;
    };

    struct EffectivePackage {
        PackageId package;
        std::vector<EffectiveProduct> products;
        std::vector<EffectiveTarget> targets;
    };

    struct ProductRealizationContext {
        std::string profile = "debug";
        std::string target_os;
    };

    [[nodiscard]] std::string host_target_os();
    [[nodiscard]] Result<EffectivePackage> realize_package(
        const Graph& graph,
        PackageId package,
        const ProductRealizationContext& context = {},
        FileCatalog* files = nullptr
    );
}
