#include "portable_project.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <ranges>

namespace kaixa::plugin::cmake::detail {
    namespace {
        std::string literal(const std::string_view value) {
            std::string equals;
            while (value.contains("]" + equals + "]"))
                equals += '=';

            return "[" + equals + "[" + std::string(value) + "]" + equals + "]";
        }

        std::string expanding_literal(const std::string_view value) {
            std::string result = "\"";
            for (const char character: value) {
                if (character == '\\' || character == '"')
                    result.push_back('\\');

                result.push_back(character);
            }
            result.push_back('"');
            return result;
        }

        const std::string* string_at(const Value* table, const std::string_view key) {
            const Value* value = table ? table->find(key) : nullptr;
            return value ? value->as_string() : nullptr;
        }

        std::string option_value(const Value& value) {
            if (const bool* boolean = value.as_boolean())
                return *boolean ? "ON" : "OFF";

            if (const std::int64_t* integer = value.as_integer())
                return std::to_string(*integer);

            if (const double* floating = value.as_floating()) {
                std::array<char, 64> buffer{};
                const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), *floating);
                return std::string(buffer.data(), converted.ptr);
            }
            return literal(*value.as_string());
        }

        const Value* consumer(const PackageNode& package) {
            return package.descriptor ? package.descriptor->find("consumer") : nullptr;
        }

        std::filesystem::path consumer_path(const PackageNode& package) {
            const std::string* path = string_at(consumer(package), "path");
            return path ? std::filesystem::path(*path) : std::filesystem::path{};
        }

        bool source_only(const PackageNode& package) {
            const std::string* kind = string_at(package.descriptor ? &*package.descriptor : nullptr, "kind");
            return kind && *kind == "source-only";
        }

        std::string portable_path(const PackageNode& package, const std::filesystem::path& path) {
            std::string result = "${" + source_variable(package.id) + "}";
            const std::filesystem::path relative = path.lexically_relative(package.directory);
            if (!relative.empty() && relative != ".")
                result += "/" + relative.generic_string();

            return result;
        }

        Result<void> append_source(
            std::string& result,
            const PackageNode& package,
            const std::filesystem::path& root,
            const bool add_project,
            const bool exclude_from_all
        ) {
            const std::string variable = source_variable(package.id);
            if (!package.source || !package.source->locator || package.source->locator->driver == "path") {
                const std::filesystem::path relative = package.directory.lexically_relative(root);
                result += "set(" + variable + " " + expanding_literal("${CMAKE_CURRENT_LIST_DIR}/" + relative.generic_string()) + ")\n";
                if (add_project) {
                    result += "add_subdirectory("
                        + expanding_literal("${" + variable + "}")
                        + " "
                        + expanding_literal("${CMAKE_BINARY_DIR}/_kaixa/" + package.name);
                    if (exclude_from_all)
                        result += " EXCLUDE_FROM_ALL";

                    result += ")\n";
                }
                return {};
            }

            const SourceLocator& locator = *package.source->locator;
            const std::string content_name = "kaixa_package_" + std::to_string(package.id.index);
            const std::string* url = string_at(&locator.options, "url");
            if (!url) {
                return std::unexpected(error_at(
                    locator.options.location(),
                    "portable CMake generation requires a URL for source driver `" + locator.driver + "`"
                ));
            }

            result += "FetchContent_Declare(" + content_name + "\n";
            if (locator.driver == "git") {
                result += "    GIT_REPOSITORY " + literal(*url) + "\n";
                const std::string* revision = package.source->identity ? &*package.source->identity : nullptr;
                for (const std::string_view key: {"rev", "tag", "branch"}) {
                    if (!revision)
                        revision = string_at(&locator.options, key);
                }
                if (!revision) {
                    return std::unexpected(
                        error_at(locator.options.location(), "portable CMake generation requires a pinned Git revision")
                    );
                }
                result += "    GIT_TAG " + literal(*revision) + "\n";
            } else if (locator.driver == "archive" || locator.driver == "url") {
                result += "    URL " + literal(*url) + "\n";
                const std::string* integrity = package.source->integrity ? &*package.source->integrity : nullptr;
                if (!integrity)
                    integrity = string_at(&locator.options, "sha256");
                if (integrity) {
                    const std::string_view digest = integrity->starts_with("sha256:")
                        ? std::string_view(*integrity).substr(std::string_view("sha256:").size())
                        : std::string_view(*integrity);
                    result += "    URL_HASH SHA256=" + std::string(digest) + "\n";
                }
                result += "    DOWNLOAD_EXTRACT_TIMESTAMP TRUE\n";
            } else {
                return std::unexpected(error_at(
                    locator.options.location(),
                    "source driver `" + locator.driver + "` cannot be represented by portable CMake generation"
                ));
            }

            const std::filesystem::path subdirectory = consumer_path(package);
            result += ")\n";
            result += "FetchContent_GetProperties(" + content_name + ")\n";
            result += "if(NOT " + content_name + "_POPULATED)\n";
            result += "    FetchContent_Populate(" + content_name + ")\n";
            result += "endif()\n";
            std::string source = "${" + content_name + "_SOURCE_DIR}";
            if (!subdirectory.empty())
                source += "/" + subdirectory.generic_string();

            result += "set(" + variable + " " + expanding_literal(source) + ")\n";
            if (add_project) {
                result += "add_subdirectory("
                    + expanding_literal("${" + variable + "}")
                    + " "
                    + expanding_literal("${CMAKE_BINARY_DIR}/_kaixa/" + package.name);
                if (exclude_from_all)
                    result += " EXCLUDE_FROM_ALL";

                result += ")\n";
            }
            return {};
        }

        void append_aliases(std::string& result, const PackageNode& package) {
            const Value* aliases = consumer(package) ? consumer(package)->find("aliases") : nullptr;
            if (!aliases)
                return;

            for (const TableEntry& alias: *aliases->as_table()) {
                result += "if(NOT TARGET " + alias.key + ")\n";
                result += "    add_library(" + alias.key + " INTERFACE)\n";
                result += "    target_link_libraries(" + alias.key + " INTERFACE " + *alias.value.as_string() + ")\n";
                result += "endif()\n";
            }
        }

        void append_target_patch(std::string& result, const PackageNode& package, const TableEntry& patch) {
            const auto append_values = [&](const std::string_view key, const std::string_view command, const std::string_view scope) {
                const Value* declared = patch.value.find(key);
                if (!declared)
                    return;

                result += std::string(command) + "(" + patch.key + " " + std::string(scope);
                for (const Value& value: *declared->as_array())
                    result += " " + literal(*value.as_string());
                result += ")\n";
            };
            append_values("compile-options", "target_compile_options", "PRIVATE");
            append_values("defines", "target_compile_definitions", "PRIVATE");
            append_values("compile-features", "target_compile_features", "PUBLIC");

            if (const Value* includes = patch.value.find("system-include")) {
                result += "target_include_directories(" + patch.key + " SYSTEM PUBLIC";
                for (const Value& include: *includes->as_array()) {
                    result += " "
                        + expanding_literal(portable_path(package, package.directory / std::filesystem::path(*include.as_string())));
                }
                result += ")\n";
            }
            const Value* excluded = patch.value.find("exclude-from-all");
            if (excluded && *excluded->as_boolean())
                result += "set_target_properties(" + patch.key + " PROPERTIES EXCLUDE_FROM_ALL TRUE)\n";
        }

        void append_consumer_adjustments(std::string& result, const PackageNode& package) {
            const Value* declared_consumer = consumer(package);
            if (!declared_consumer)
                return;

            append_aliases(result, package);
            if (const Value* includes = declared_consumer->find("feature-includes")) {
                for (const std::string& feature: package.active_features) {
                    const Value* paths = includes->find(feature);
                    if (!paths)
                        continue;

                    for (const Value& path: *paths->as_array()) {
                        result += "include(" + expanding_literal(portable_path(package, package.directory / *path.as_string())) + ")\n";
                    }
                }
            }
            if (const Value* patches = declared_consumer->find("patch-targets")) {
                for (const TableEntry& patch: *patches->as_table())
                    append_target_patch(result, package, patch);
            }
            if (const Value* feature_patches = declared_consumer->find("feature-patch-targets")) {
                for (const std::string& feature: package.active_features) {
                    const Value* patches = feature_patches->find(feature);
                    if (!patches)
                        continue;

                    for (const TableEntry& patch: *patches->as_table())
                        append_target_patch(result, package, patch);
                }
            }
        }

        void append_source_only_target(std::string& result, const PackageNode& package) {
            if (!source_only(package))
                return;

            const Value* products = package.descriptor->find("products");
            const std::string* product = string_at(products, "default");
            if (!product)
                return;

            result += "if(NOT TARGET " + *product + ")\n";
            result += "    add_library(" + *product + " INTERFACE IMPORTED GLOBAL)\n";
            for (const auto& [key, system]: {std::pair{"include", false}, std::pair{"system-include", true}}) {
                const Value* paths = package.descriptor->find(key);
                if (!paths)
                    continue;

                result += "    target_include_directories(" + *product + (system ? " SYSTEM INTERFACE" : " INTERFACE");
                for (const Value& path: *paths->as_array()) {
                    result += " " + expanding_literal(portable_path(package, package.directory / *path.as_string()));
                }
                result += ")\n";
            }
            result += "endif()\n\n";
        }
    }

    Result<std::string> generate_portable_dependencies(
        const Graph& graph,
        const PackageNode& package,
        const Options& options,
        const std::span<const PortableProject> projects
    ) {
        std::string result = "# Generated by Kaixa from Kaixa.toml. Do not edit.\n"
                             "if(KAIXA_CMAKE_DEPENDENCIES_INCLUDED)\n"
                             "    return()\n"
                             "endif()\n"
                             "set(KAIXA_CMAKE_DEPENDENCIES_INCLUDED TRUE)\n"
                             "include(FetchContent)\n"
                             "if(POLICY CMP0169)\n"
                             "    cmake_policy(SET CMP0169 OLD)\n"
                             "endif()\n\n";

        for (const PackageNode& candidate: graph.nodes()) {
            if (candidate.id == package.id || !candidate.source || candidate.kind != PackageKind::opaque)
                continue;

            auto appended = append_source(result, candidate, options.source, false, true);
            if (!appended)
                return std::unexpected(appended.error());

            result += '\n';
        }

        std::vector<std::filesystem::path> added_projects = {options.source};
        for (const PortableProject& project: projects) {
            if (project.package == package.id)
                continue;

            const PackageNode& dependency = graph[project.package];
            if (std::ranges::find(added_projects, project.source) != added_projects.end())
                continue;

            added_projects.push_back(project.source);

            const Value* declared_options = consumer(dependency) ? consumer(dependency)->find("options") : nullptr;
            if (declared_options) {
                for (const TableEntry& option: *declared_options->as_table()) {
                    result += "set(" + option.key + " " + option_value(option.value) + " CACHE INTERNAL \"Set by Kaixa\" FORCE)\n";
                }
            }

            auto appended = append_source(result, dependency, options.source, true, project.exclude_from_all);
            if (!appended)
                return std::unexpected(appended.error());

            append_consumer_adjustments(result, dependency);
            result += '\n';
        }

        for (const PackageNode& candidate: graph.nodes())
            append_source_only_target(result, candidate);

        return result;
    }
}
