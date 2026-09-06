#include "integration.hpp"

#include <generation/cmake_syntax.hpp>
#include <generation/project.hpp>

#include <algorithm>
#include <filesystem>
#include <set>
#include <string_view>
#include <utility>

namespace kaixa::plugin::cmake::detail {
    namespace {
        bool valid_cmake_variable(const std::string_view name) {
            if (name.empty() || (!std::isalpha(static_cast<unsigned char>(name.front())) && name.front() != '_'))
                return false;

            return std::ranges::all_of(name.substr(1), [](const unsigned char character) {
                return std::isalnum(character) || character == '_';
            });
        }

        Result<std::vector<TableEntry>> consumer_options(const PackageNode& package) {
            if (!package.descriptor())
                return std::vector<TableEntry>{};

            const Value* consumer = package.descriptor()->find("consumer");
            if (!consumer)
                return std::vector<TableEntry>{};

            const Value* options = consumer->find("options");
            if (!options)
                return std::vector<TableEntry>{};

            const std::vector<TableEntry>* entries = options->as_table();
            if (!entries)
                return std::unexpected(error_at(options->location(), "CMake consumer `options` must be a table"));

            for (const TableEntry& entry: *entries) {
                if (!valid_cmake_variable(entry.key)) {
                    return std::unexpected(error_at(entry.value.location(), "`" + entry.key + "` is not a valid CMake option name"));
                }
            }
            return *entries;
        }

        Result<std::vector<std::filesystem::path>> consumer_feature_includes(const PackageNode& package) {
            if (!package.descriptor())
                return std::vector<std::filesystem::path>{};

            const Value* consumer = package.descriptor()->find("consumer");
            const Value* declared = consumer ? consumer->find("feature-includes") : nullptr;
            if (!declared)
                return std::vector<std::filesystem::path>{};

            const std::vector<TableEntry>* features = declared->as_table();
            if (!features)
                return std::unexpected(error_at(declared->location(), "CMake consumer `feature-includes` must be a table"));

            std::vector<std::filesystem::path> result;
            for (const std::string& active: package.active_features) {
                const auto feature = std::ranges::find(*features, active, &TableEntry::key);
                if (feature == features->end())
                    continue;

                const std::vector<Value>* paths = feature->value.as_array();
                if (!paths) {
                    return std::unexpected(error_at(feature->value.location(), "CMake consumer feature includes must be arrays of paths"));
                }
                for (const Value& value: *paths) {
                    const std::string* text = value.as_string();
                    if (!text || text->empty())
                        return std::unexpected(error_at(value.location(), "CMake consumer feature include must be a non-empty path"));

                    const std::filesystem::path relative = *text;
                    if (relative.is_absolute() || relative.has_root_path() || std::ranges::find(relative, "..") != relative.end()) {
                        return std::unexpected(
                            error_at(value.location(), "CMake consumer feature include must stay inside the adopted source tree")
                        );
                    }

                    const std::filesystem::path path = (package.directory / relative).lexically_normal();
                    if (!std::filesystem::is_regular_file(path)) {
                        return std::unexpected(
                            error_at(value.location(), "CMake consumer feature include does not exist: " + path.string())
                        );
                    }
                    result.push_back(path);
                }
            }
            return result;
        }

        Result<std::vector<std::pair<std::string, std::string>>> consumer_aliases(const PackageNode& package) {
            if (!package.descriptor())
                return std::vector<std::pair<std::string, std::string>>{};

            const Value* consumer = package.descriptor()->find("consumer");
            const Value* declared = consumer ? consumer->find("aliases") : nullptr;
            if (!declared)
                return std::vector<std::pair<std::string, std::string>>{};

            const std::vector<TableEntry>* aliases = declared->as_table();
            if (!aliases)
                return std::unexpected(error_at(declared->location(), "CMake consumer `aliases` must be a table"));

            std::vector<std::pair<std::string, std::string>> result;
            result.reserve(aliases->size());
            for (const TableEntry& alias: *aliases) {
                const std::string* target = alias.value.as_string();
                if (alias.key.empty() || !target || target->empty()) {
                    return std::unexpected(error_at(alias.value.location(), "CMake consumer aliases require non-empty target names"));
                }
                result.emplace_back(alias.key, *target);
            }
            return result;
        }

        struct ConsumerTargetPatch {
            std::string target;
            std::vector<std::string> compile_options;
            std::vector<std::string> definitions;
            std::vector<std::string> compile_features;
            std::vector<std::filesystem::path> system_includes;
            bool exclude_from_all = false;
        };

        Result<std::vector<std::string>> target_patch_values(const Value* declared, const std::string_view description) {
            if (!declared)
                return std::vector<std::string>{};

            const std::vector<Value>* values = declared->as_array();
            if (!values)
                return std::unexpected(error_at(declared->location(), std::string(description) + " must be an array"));

            std::vector<std::string> result;
            result.reserve(values->size());
            for (const Value& value: *values) {
                const std::string* text = value.as_string();
                if (!text || text->empty())
                    return std::unexpected(error_at(value.location(), std::string(description) + " must contain non-empty strings"));

                result.push_back(*text);
            }
            return result;
        }

        Result<void> append_consumer_target_patches(
            std::vector<ConsumerTargetPatch>& result,
            const PackageNode& package,
            const Value& declared
        ) {
            const std::vector<TableEntry>* targets = declared.as_table();
            if (!targets)
                return std::unexpected(error_at(declared.location(), "CMake consumer target patches must be a table"));

            for (const TableEntry& target: *targets) {
                if (target.key.empty() || !target.value.as_table())
                    return std::unexpected(error_at(target.value.location(), "CMake consumer target patch must be a table"));

                auto options = target_patch_values(target.value.find("compile-options"), "target patch compile options");
                if (!options)
                    return std::unexpected(options.error());
                auto definitions = target_patch_values(target.value.find("defines"), "target patch definitions");
                if (!definitions)
                    return std::unexpected(definitions.error());
                auto features = target_patch_values(target.value.find("compile-features"), "target patch compile features");
                if (!features)
                    return std::unexpected(features.error());

                auto includes = target_patch_values(target.value.find("system-include"), "target patch system includes");
                if (!includes)
                    return std::unexpected(includes.error());
                std::vector<std::filesystem::path> resolved_includes;
                resolved_includes.reserve(includes->size());
                for (const std::string& include: *includes) {
                    const std::filesystem::path relative = include;
                    if (relative.is_absolute() || relative.has_root_path() || std::ranges::find(relative, "..") != relative.end()) {
                        return std::unexpected(
                            error_at(target.value.location(), "target patch system include must stay inside the adopted source tree")
                        );
                    }
                    resolved_includes.push_back((package.directory / relative).lexically_normal());
                }

                bool exclude_from_all = false;
                if (const Value* excluded = target.value.find("exclude-from-all")) {
                    const bool* enabled = excluded->as_boolean();
                    if (!enabled)
                        return std::unexpected(error_at(excluded->location(), "target patch `exclude-from-all` must be a boolean"));

                    exclude_from_all = *enabled;
                }

                result.push_back(
                    {target.key,
                        std::move(*options),
                        std::move(*definitions),
                        std::move(*features),
                        std::move(resolved_includes),
                        exclude_from_all}
                );
            }
            return {};
        }

        Result<std::vector<ConsumerTargetPatch>> consumer_target_patches(const PackageNode& package) {
            if (!package.descriptor())
                return std::vector<ConsumerTargetPatch>{};

            const Value* consumer = package.descriptor()->find("consumer");
            if (!consumer)
                return std::vector<ConsumerTargetPatch>{};

            std::vector<ConsumerTargetPatch> result;
            if (const Value* declared = consumer->find("patch-targets")) {
                auto appended = append_consumer_target_patches(result, package, *declared);
                if (!appended)
                    return std::unexpected(appended.error());
            }

            const Value* conditional = consumer->find("feature-patch-targets");
            if (!conditional)
                return result;

            const std::vector<TableEntry>* features = conditional->as_table();
            if (!features) {
                return std::unexpected(error_at(conditional->location(), "CMake consumer `feature-patch-targets` must be a table"));
            }
            for (const std::string& active: package.active_features) {
                const auto feature = std::ranges::find(*features, active, &TableEntry::key);
                if (feature == features->end())
                    continue;

                auto appended = append_consumer_target_patches(result, package, feature->value);
                if (!appended)
                    return std::unexpected(appended.error());
            }
            return result;
        }

        Result<std::vector<std::filesystem::path>> source_only_interface_paths(const PackageNode& package, const std::string_view key) {
            const Value* declared = package.descriptor() ? package.descriptor()->find(key) : nullptr;
            if (!declared)
                return std::vector<std::filesystem::path>{};

            const std::vector<Value>* values = declared->as_array();
            if (!values)
                return std::unexpected(error_at(declared->location(), "source-only interface paths must be arrays"));

            std::vector<std::filesystem::path> result;
            result.reserve(values->size());
            for (const Value& value: *values) {
                const std::string* text = value.as_string();
                if (!text || text->empty())
                    return std::unexpected(error_at(value.location(), "source-only interface path must be a non-empty string"));

                const std::filesystem::path relative = *text;
                if (relative.is_absolute() || relative.has_root_path() || std::ranges::find(relative, "..") != relative.end()) {
                    return std::unexpected(error_at(value.location(), "source-only interface path must stay inside the package"));
                }
                result.push_back((package.directory / relative).lexically_normal());
            }
            return result;
        }

        Result<std::string> source_only_interface_integration(const Graph& graph) {
            std::string result;
            for (const PackageNode& package: graph.nodes()) {
                if (package.has_build_semantics() || !package.descriptor())
                    continue;

                const Value* kind = package.descriptor()->find("kind");
                const std::string* kind_name = kind ? kind->as_string() : nullptr;
                const Value* products = package.descriptor()->find("products");
                const Value* declared_product = products ? products->find("default") : nullptr;
                const std::string* product = declared_product ? declared_product->as_string() : nullptr;
                if (!kind_name || *kind_name != "source-only" || !product)
                    continue;
                if (product->empty() || !std::ranges::all_of(*product, [](const unsigned char character) {
                        return std::isalnum(character)
                            || character == '_'
                            || character == '.'
                            || character == ':'
                            || character == '+'
                            || character == '-';
                    })) {
                    return std::unexpected(error_at(declared_product->location(), "invalid source-only CMake interface product name"));
                }

                auto includes = source_only_interface_paths(package, "include");
                if (!includes)
                    return std::unexpected(includes.error());
                auto system_includes = source_only_interface_paths(package, "system-include");
                if (!system_includes)
                    return std::unexpected(system_includes.error());

                result += "  if(NOT TARGET " + *product + ")\n";
                result += "    add_library(" + *product + " INTERFACE IMPORTED GLOBAL)\n";
                if (!includes->empty()) {
                    result += "    target_include_directories(" + *product + " INTERFACE";
                    for (const std::filesystem::path& include: *includes)
                        result += " " + detail::syntax::path(include);
                    result += ")\n";
                }
                if (!system_includes->empty()) {
                    result += "    target_include_directories(" + *product + " SYSTEM INTERFACE";
                    for (const std::filesystem::path& include: *system_includes)
                        result += " " + detail::syntax::path(include);
                    result += ")\n";
                }
                result += "  endif()\n";
            }
            return result;
        }

        std::string product_integration(const BuildContext& context) {
            const std::filesystem::path metadata = product_metadata_directory(context);
            const std::filesystem::path dependencies = context.directory / "_dependencies";
            return "  set(_kaixa_product_directory "
                + detail::syntax::path(metadata)
                + ")\n"
                  "  set(_kaixa_dependency_binary "
                + detail::syntax::path(dependencies)
                + ")\n"
                  "  function(_kaixa_write_product _kaixa_target _kaixa_type)\n"
                  "    string(SHA256 _kaixa_id \"${_kaixa_target}\")\n"
                  "    if(_kaixa_type STREQUAL \"EXECUTABLE\" OR "
                  "_kaixa_type STREQUAL \"STATIC_LIBRARY\" OR "
                  "_kaixa_type STREQUAL \"SHARED_LIBRARY\" OR "
                  "_kaixa_type STREQUAL \"MODULE_LIBRARY\")\n"
                  "      set(_kaixa_artifact \"$<TARGET_FILE:${_kaixa_target}>\")\n"
                  "    else()\n"
                  "      set(_kaixa_artifact \"\")\n"
                  "    endif()\n"
                  "    file(GENERATE\n"
                  "      OUTPUT \"${_kaixa_product_directory}/$<CONFIG>/${_kaixa_id}.product\"\n"
                  "      CONTENT \"${_kaixa_target}\\n${_kaixa_type}\\n${_kaixa_artifact}\\n\"\n"
                  "    )\n"
                  "  endfunction()\n"
                  "  function(_kaixa_collect_products _kaixa_directory)\n"
                  "    get_property(_kaixa_targets DIRECTORY \"${_kaixa_directory}\" "
                  "PROPERTY BUILDSYSTEM_TARGETS)\n"
                  "    foreach(_kaixa_target IN LISTS _kaixa_targets)\n"
                  "      get_target_property(_kaixa_type \"${_kaixa_target}\" TYPE)\n"
                  "      _kaixa_write_product(\"${_kaixa_target}\" \"${_kaixa_type}\")\n"
                  "    endforeach()\n"
                  "    get_property(_kaixa_subdirectories DIRECTORY \"${_kaixa_directory}\" "
                  "PROPERTY SUBDIRECTORIES)\n"
                  "    foreach(_kaixa_subdirectory IN LISTS _kaixa_subdirectories)\n"
                  "      get_property(_kaixa_binary DIRECTORY \"${_kaixa_subdirectory}\" "
                  "PROPERTY BINARY_DIR)\n"
                  "      cmake_path(IS_PREFIX _kaixa_dependency_binary \"${_kaixa_binary}\" "
                  "NORMALIZE _kaixa_is_dependency)\n"
                  "      if(NOT _kaixa_is_dependency)\n"
                  "        _kaixa_collect_products(\"${_kaixa_subdirectory}\")\n"
                  "      endif()\n"
                  "    endforeach()\n"
                  "  endfunction()\n"
                  "  function(_kaixa_write_products)\n"
                  "    file(REMOVE_RECURSE \"${_kaixa_product_directory}\")\n"
                  "    file(GENERATE "
                  "OUTPUT \"${_kaixa_product_directory}/$<CONFIG>/.catalog\" CONTENT \"\")\n"
                  "    _kaixa_collect_products(\"${CMAKE_SOURCE_DIR}\")\n"
                  "  endfunction()\n"
                  "  cmake_language(DEFER DIRECTORY \"${CMAKE_SOURCE_DIR}\" "
                  "CALL _kaixa_write_products)\n";
        }

        std::string dependency_policy(const Options& options) {
            std::string result;
            if (options.msvc_runtime != MsvcRuntime::default_runtime) {
                const std::string runtime = options.msvc_runtime == MsvcRuntime::static_runtime ? "MultiThreaded" : "MultiThreadedDLL";
                result += "  set(CMAKE_MSVC_RUNTIME_LIBRARY \"" + runtime + "$<$<CONFIG:Debug>:Debug>\")\n";
            }
            if (options.cxx_standard) {
                result += "  set(CMAKE_CXX_STANDARD " + std::to_string(*options.cxx_standard) + ")\n";
                result += "  set(CMAKE_CXX_EXTENSIONS OFF)\n";
            }
            return result;
        }

    }

    Result<std::string> dependency_integration(const DependencyIntegrationContext& context) {
        detail::syntax::Writer writer;
        writer.line("# Generated by Kaixa.");
        writer.line("if(KAIXA_CMAKE_PREFIX_PATH)");
        writer.indent();
        writer.line("list(PREPEND CMAKE_PREFIX_PATH ${KAIXA_CMAKE_PREFIX_PATH})");
        writer.outdent();
        writer.line("endif()");
        writer.line("if(NOT KAIXA_CMAKE_DEPENDENCIES_INCLUDED)");
        writer.indent();
        writer.line("set(KAIXA_CMAKE_DEPENDENCIES_INCLUDED TRUE)");
        writer.append(dependency_policy(context.build.project));
        writer.append(product_integration(context.build));
        for (const PackageNode& package: context.graph.nodes()) {
            if (package.source && !package.directory.empty()) {
                writer.line("set(" + detail::source_variable(package.id) + " " + detail::syntax::path(package.directory) + ")");
            }
        }
        auto source_only_interfaces = source_only_interface_integration(context.graph);
        if (!source_only_interfaces)
            return std::unexpected(source_only_interfaces.error());

        writer.append(*source_only_interfaces);
        std::set<std::filesystem::path> added_projects;
        added_projects.insert(context.projects[context.package.id.index]->source);
        for (const PackageId id: context.source_packages) {
            if (id == context.package.id)
                continue;

            if (!added_projects.insert(context.projects[id.index]->source).second)
                continue;

            const PackageNode& dependency = context.graph[id];
            auto options = consumer_options(dependency);
            if (!options)
                return std::unexpected(options.error());

            auto feature_includes = consumer_feature_includes(dependency);
            if (!feature_includes)
                return std::unexpected(feature_includes.error());

            auto aliases = consumer_aliases(dependency);
            if (!aliases)
                return std::unexpected(aliases.error());

            auto target_patches = consumer_target_patches(dependency);
            if (!target_patches)
                return std::unexpected(target_patches.error());

            if (!options->empty()) {
                writer.line("block()");
                writer.indent();
                for (const TableEntry& option: *options) {
                    auto value = detail::syntax::scalar(option.value);
                    if (!value)
                        return std::unexpected(value.error());

                    writer.line("set(" + option.key + " " + *value + " CACHE INTERNAL \"Set by Kaixa\" FORCE)");
                }
            }

            std::string add_subdirectory = "add_subdirectory("
                + detail::syntax::path(context.projects[id.index]->source)
                + " "
                + detail::syntax::path(context.build.directory / "_dependencies" / dependency.name);
            if (!context.normal_source_visited[id.index])
                add_subdirectory += " EXCLUDE_FROM_ALL";

            writer.line(add_subdirectory + ")");
            for (const auto& [alias, target]: *aliases) {
                writer.line("add_library(" + alias + " INTERFACE)");
                std::string link_alias = "target_link_libraries(" + alias;
                link_alias += " INTERFACE ";
                link_alias += target;
                link_alias += ')';
                writer.line(link_alias);
            }
            for (const std::filesystem::path& include: *feature_includes)
                writer.line("include(" + detail::syntax::path(include) + ")");

            for (const ConsumerTargetPatch& patch: *target_patches) {
                const auto append_values =
                    [&](const std::string_view command, const std::string_view scope, const std::vector<std::string>& values) {
                        if (values.empty())
                            return;

                        std::string line = std::string(command) + "(" + patch.target + " " + std::string(scope);
                        for (const std::string& value: values)
                            line += " " + detail::syntax::literal(value);

                        writer.line(line + ")");
                    };
                append_values("target_compile_options", "PRIVATE", patch.compile_options);
                append_values("target_compile_definitions", "PRIVATE", patch.definitions);
                append_values("target_compile_features", "PUBLIC", patch.compile_features);
                if (!patch.system_includes.empty()) {
                    std::string line = "target_include_directories(" + patch.target + " SYSTEM PUBLIC";
                    for (const std::filesystem::path& include: patch.system_includes)
                        line += " " + detail::syntax::path(include);

                    writer.line(line + ")");
                }
                if (patch.exclude_from_all)
                    writer.line("set_target_properties(" + patch.target + " PROPERTIES EXCLUDE_FROM_ALL TRUE)");
            }
            if (!options->empty()) {
                writer.outdent();
                writer.line("endblock()");
            }
        }
        writer.outdent();
        writer.line("endif()");
        return std::move(writer).finish();
    }
}
