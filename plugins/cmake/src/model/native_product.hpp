#pragma once

#include <kaixa/model/effective_product.hpp>
#include <kaixa/model/file_set.hpp>
#include <kaixa/model/graph.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace kaixa::plugin::cmake::detail {
    // The native half of a product declaration. Kaixa carries these keys as opaque resolver
    // options; this resolver is their owner and converts them once, here.
    struct NativeProductOptions {
        FileSet headers;
        FileSet public_headers;
        std::vector<std::string> include_directories;
        std::vector<std::string> public_include_directories;
        std::vector<std::string> system_include_directories;
        std::vector<std::string> public_system_include_directories;
        std::vector<TableEntry> definitions;
        std::vector<TableEntry> public_definitions;
        std::vector<std::string> system_libraries;
        std::vector<std::filesystem::path> dependency_source_files;
    };

    [[nodiscard]] Result<NativeProductOptions> read_native_product_options(
        const EffectiveProduct& product,
        const Graph& graph,
        const PackageNode& package,
        FileCatalog* files
    );
}
