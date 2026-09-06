#include "native_product.hpp"

#include <kaixa/config/table_reader.hpp>
#include <kaixa/model/dependency_paths.hpp>

#include <algorithm>
#include <string_view>
#include <utility>

namespace kaixa::plugin::cmake::detail {
    namespace {
        Result<std::vector<TableEntry>> read_definitions(TableReader& table, const std::string_view key) {
            const Value* declared = table.take(key);
            if (!declared)
                return std::vector<TableEntry>{};

            const std::vector<TableEntry>* entries = declared->as_table();
            if (!entries) {
                return std::unexpected(error_at(declared->location(), "product definitions must be a table"));
            }
            return *entries;
        }

        Result<void> resolve_dependency_directories(
            std::vector<std::string>& directories,
            const Graph& graph,
            const PackageNode& package,
            const SourceLocation& location
        ) {
            constexpr std::string_view prefix = "${dependency:";
            for (std::string& directory: directories) {
                if (!directory.starts_with(prefix) || !directory.ends_with('}'))
                    continue;

                const std::string_view local_name(directory.data() + prefix.size(), directory.size() - prefix.size() - 1);
                auto dependency = find_dependency_by_local_name(graph, package, local_name, location);
                if (!dependency)
                    return std::unexpected(dependency.error());

                auto source_directory = dependency_source_directory(**dependency, location);
                if (!source_directory)
                    return std::unexpected(source_directory.error());

                directory = source_directory->generic_string();
            }
            return {};
        }

        Result<std::vector<std::filesystem::path>> resolve_dependency_sources(
            const std::vector<std::string>& declared_sources,
            const Graph& graph,
            const PackageNode& package,
            const SourceLocation& location
        ) {
            std::vector<std::filesystem::path> result;
            for (const std::string& declared: declared_sources) {
                const std::size_t separator = declared.find(':');
                if (separator == std::string::npos || separator == 0 || separator + 1 == declared.size()) {
                    return std::unexpected(error_at(location, "dependency source `" + declared + "` must use `dependency:path`"));
                }
                auto dependency = find_dependency_by_local_name(graph, package, std::string_view(declared).substr(0, separator), location);
                if (!dependency)
                    return std::unexpected(dependency.error());

                auto directory = dependency_source_directory(**dependency, location);
                if (!directory)
                    return std::unexpected(directory.error());

                result.push_back((*directory / std::filesystem::path(declared.substr(separator + 1))).lexically_normal());
            }
            return result;
        }

        Result<void> read_include_directories(TableReader& table, NativeProductOptions& result) {
            for (
                const auto& [key, output]: {std::pair{std::string_view{"include"}, &result.include_directories},
                    std::pair{std::string_view{"public-include"}, &result.public_include_directories},
                    std::pair{std::string_view{"system-include"}, &result.system_include_directories},
                    std::pair{std::string_view{"public-system-include"}, &result.public_system_include_directories}}
            ) {
                auto values = table.string_array(key);
                if (!values)
                    return std::unexpected(values.error());

                *output = std::move(*values);
            }
            return {};
        }

        Result<void> expand_headers(NativeProductOptions& result, const PackageNode& package, FileCatalog* files) {
            auto header_files = expand_file_set(result.headers, package.directory, package.directory, true, files);
            if (!header_files)
                return std::unexpected(header_files.error());

            result.headers.files = std::move(*header_files);
            auto public_header_files = expand_file_set(result.public_headers, package.directory, package.directory, true, files);
            if (!public_header_files)
                return std::unexpected(public_header_files.error());

            result.public_headers.files = std::move(*public_header_files);
            std::erase_if(result.headers.files, [&](const std::filesystem::path& header) {
                return std::ranges::find(result.public_headers.files, header) != result.public_headers.files.end();
            });
            return {};
        }
    }

    Result<NativeProductOptions> read_native_product_options(
        const EffectiveProduct& product,
        const Graph& graph,
        const PackageNode& package,
        FileCatalog* files
    ) {
        const std::string path = product.type == EffectiveProductType::executable ? "bin" : "lib";
        auto table_result = TableReader::bind(product.resolver_options, path);
        if (!table_result)
            return std::unexpected(table_result.error());

        TableReader table = std::move(*table_result);
        NativeProductOptions result;
        result.headers.location = product.location;
        result.public_headers.location = product.location;

        for (
            const auto& [key, output]:
            {std::pair{std::string_view{"headers"}, &result.headers}, std::pair{std::string_view{"public-headers"}, &result.public_headers}}
        ) {
            auto values = table.string_array(key);
            if (!values)
                return std::unexpected(values.error());

            output->include = std::move(*values);
        }

        auto includes = read_include_directories(table, result);
        if (!includes)
            return std::unexpected(includes.error());

        auto private_definitions = read_definitions(table, "defines");
        if (!private_definitions)
            return std::unexpected(private_definitions.error());

        result.definitions = std::move(*private_definitions);
        auto public_definitions = read_definitions(table, "public-defines");
        if (!public_definitions)
            return std::unexpected(public_definitions.error());

        result.public_definitions = std::move(*public_definitions);

        auto system_libraries = table.string_array("system-libraries");
        if (!system_libraries)
            return std::unexpected(system_libraries.error());

        result.system_libraries = std::move(*system_libraries);

        auto declared_dependency_sources = table.string_array("dependency-sources");
        if (!declared_dependency_sources)
            return std::unexpected(declared_dependency_sources.error());

        auto finished = table.finish();
        if (!finished)
            return std::unexpected(finished.error());

        auto dependency_source_files = resolve_dependency_sources(*declared_dependency_sources, graph, package, product.location);
        if (!dependency_source_files)
            return std::unexpected(dependency_source_files.error());

        result.dependency_source_files = std::move(*dependency_source_files);
        for (
            std::vector<std::string>* directories: {&result.include_directories,
                &result.public_include_directories,
                &result.system_include_directories,
                &result.public_system_include_directories}
        ) {
            auto resolved = resolve_dependency_directories(*directories, graph, package, product.location);
            if (!resolved)
                return std::unexpected(resolved.error());
        }

        auto expanded = expand_headers(result, package, files);
        if (!expanded)
            return std::unexpected(expanded.error());

        return result;
    }
}
