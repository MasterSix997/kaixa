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

    struct EffectiveProduct {
        std::string name;
        EffectiveProductType type = EffectiveProductType::static_library;
        FileSet sources;
        FileSet headers;
        FileSet public_headers;
        std::vector<std::string> include_directories;
        std::vector<std::string> public_include_directories;
        std::vector<std::string> system_include_directories;
        std::vector<std::string> public_system_include_directories;
        std::vector<TableEntry> definitions;
        std::vector<TableEntry> public_definitions;
        std::vector<std::string> system_libraries;
        std::vector<std::string> dependency_sources;
        std::vector<std::filesystem::path> dependency_source_files;
        FileSet runtime_files;
        std::vector<Value> policy_layers;
        bool modules = false;
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
