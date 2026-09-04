#include <kaixa/plugin/cmake/resolver.hpp>

#include <kaixa/config/value_operations.hpp>
#include <kaixa/extension/registry.hpp>
#include <kaixa/foundation/process.hpp>

#include "cmake_syntax.hpp"
#include "configuration.hpp"
#include "file_api.hpp"
#include "portable_project.hpp"
#include "testing.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kaixa::plugin::cmake {
    namespace {
        using detail::ConfigurationCache;
        using detail::dependency_mode;
        using detail::DependencyMode;
        using detail::GenerationMode;
        using detail::MsvcRuntime;
        using detail::Options;
        using detail::read_build_options;
        using detail::read_cached_options;

        std::string configuration_name(const std::string& profile) {
            if (profile == "debug")
                return "Debug";

            if (profile == "release")
                return "Release";

            if (profile == "relwithdebinfo")
                return "RelWithDebInfo";

            if (profile == "minsizerel")
                return "MinSizeRel";

            return profile;
        }

        std::optional<std::string> requested_generator(const std::vector<std::string>& arguments) {
            for (std::size_t index = 0; index < arguments.size(); ++index) {
                const std::string& argument = arguments[index];
                if ((argument == "-G" || argument == "--generator") && index + 1 < arguments.size()) {
                    return arguments[index + 1];
                }
                if (argument.starts_with("-G") && argument.size() > 2)
                    return argument.substr(2);

                if (argument.starts_with("--generator="))
                    return argument.substr(std::string("--generator=").size());
            }
            return std::nullopt;
        }

        bool uses_multiple_configurations(const std::optional<std::string>& requested) {
            const std::optional<std::string> environment = environment_variable("CMAKE_GENERATOR");
            std::string_view generator;
            if (requested) {
                generator = *requested;
            } else if (environment) {
                generator = *environment;
            }

            if (!generator.empty()) {
                return generator.contains("Visual Studio") || generator.contains("Xcode") || generator.contains("Multi-Config");
            }

#ifdef _WIN32
            return true;
#else
            return false;
#endif
        }

        struct BuildVariant {
            std::string label;
            std::string fingerprint;
            std::string directory;
        };

        std::string variant_label(const EffectiveBuildConfiguration& configuration) {
            std::string label;
            const std::size_t visible = std::min(configuration.selected.size(), std::size_t{2});
            for (std::size_t index = 0; index < visible; ++index) {
                if (!label.empty())
                    label += '+';

                label += configuration.selected[index];
            }
            if (configuration.selected.size() > visible)
                label += '+' + std::to_string(configuration.selected.size() - visible);

            if (label.empty())
                label = configuration.profile;

            // A command-line profile is the effective build identity.  Keep the
            // resolver configuration name (it still supplies compiler/generator
            // settings), but do not put a stale profile such as `clang-debug` in
            // the output path when `--profile release` was requested.
            if (configuration.profile_origin.source == "command line" && !configuration.selected.empty()) {
                constexpr std::array<std::string_view, 4> known_profiles = {"debug", "release", "relwithdebinfo", "minsizerel"};
                bool replaced = false;
                for (const std::string_view known: known_profiles) {
                    const std::string suffix = "-" + std::string(known);
                    if (label.ends_with(suffix)) {
                        label.erase(label.size() - suffix.size());
                        label += "-" + configuration.profile;
                        replaced = true;
                        break;
                    }
                }
                if (!replaced)
                    label += "+" + configuration.profile;
            }

            for (char& character: label) {
                const bool valid = (character >= 'a' && character <= 'z')
                    || (character >= 'A' && character <= 'Z')
                    || (character >= '0' && character <= '9')
                    || character == '-'
                    || character == '_'
                    || character == '+';
                if (!valid)
                    character = '_';
            }
            if (label.size() > 48)
                label = label.substr(0, 45) + "...";

            return label;
        }

        BuildVariant build_variant(
            const BuildEnvironment& environment,
            const detail::BuildOptions& options,
            const std::vector<std::string>& arguments,
            const Options& project,
            const ConfiguredPackageInstance& instance,
            const bool primary
        ) {
            std::uint64_t hash = 14695981039346656037ull;
            const auto absorb = [&](const std::string_view value) {
                for (const char character: value) {
                    const auto byte = static_cast<unsigned char>(character);
                    hash ^= byte;
                    hash *= 1099511628211ull;
                }
                hash ^= 0xffu;
                hash *= 1099511628211ull;
            };

            absorb(environment.configuration.profile);
            absorb(project.source.generic_string());
            absorb(project.generation == GenerationMode::export_project ? "export" : "state");
            absorb(project.policy_fingerprint);
            absorb(instance.artifact);
            if (options.generator)
                absorb(*options.generator);

            if (options.c_compiler)
                absorb(*options.c_compiler);

            if (options.cxx_compiler)
                absorb(*options.cxx_compiler);

            if (options.toolchain)
                absorb(options.toolchain->generic_string());

            for (const std::string& argument: arguments)
                absorb(argument);

            char encoded[16];
            const auto converted = std::to_chars(encoded, encoded + sizeof(encoded), hash, 16);
            const std::string fingerprint(encoded, converted.ptr);
            const std::string label = variant_label(environment.configuration);
            const std::string directory = primary ? label : label + "/instances/" + instance.artifact;
            return {label, fingerprint, directory};
        }

        std::filesystem::path cmake_build_root(const BuildEnvironment& environment, const std::string_view variant) {
            return environment.state_root / "build" / "cmake" / variant;
        }

        std::filesystem::path artifact_directory(const BuildEnvironment& environment, const ConfiguredPackageInstance& instance) {
            return environment.state_root / "cache" / "cmake" / instance.artifact;
        }

        std::optional<std::filesystem::path> install_destination(
            const BuildRequest& request,
            const bool dependency_install,
            const BuildEnvironment& environment,
            const ConfiguredPackageInstance& instance
        ) {
            if (!request.install && !dependency_install)
                return std::nullopt;

            if (request.install_prefix)
                return *request.install_prefix;

            return artifact_directory(environment, instance);
        }

        struct ConfiguredRoute {
            std::string context;
            const ConfiguredPackageInstance* instance = nullptr;
            BuildRequest request;
        };

        struct RoutePlanningContext {
            const Graph& graph;
            const ExtensionRegistry& registry;
            const PackageNode& package;
            const BuildEnvironment& environment;
            std::span<const ConfiguredPackageInstance> instances;
            BuildPlan& plan;
            ConfigurationCache& cache;
        };

        Result<std::vector<ConfiguredRoute>> configured_routes(
            const PackageNode& package,
            const BuildRequest& request,
            const std::span<const ConfiguredPackageInstance> instances
        ) {
            const std::string default_context = package.name + ":default";
            const ConfiguredPackageInstance* default_instance = find_configured_package_instance(instances, package.id, default_context);
            if (!default_instance) {
                return std::unexpected(error("configured default instance is missing for package `" + package.name + "`"));
            }

            std::vector<ConfiguredRoute> routes;
            const auto route_for = [&](const ConfiguredPackageInstance& instance, std::string context) -> ConfiguredRoute& {
                const auto existing = std::ranges::find_if(routes, [&](const ConfiguredRoute& route) {
                    return route.instance->artifact == instance.artifact;
                });
                if (existing != routes.end())
                    return *existing;

                BuildRequest routed;
                routed.jobs = request.jobs;
                routed.build_default = false;
                routed.install = request.install;
                routed.install_prefix = request.install_prefix;
                routes.push_back({std::move(context), &instance, std::move(routed)});
                return routes.back();
            };

            if (request.build_default)
                route_for(*default_instance, default_context).request.build_default = true;

            for (const std::string& target_name: request.targets) {
                const ConfiguredPackageInstance* instance = default_instance;
                std::string context = default_context;
                if (package.manifest) {
                    const auto target = std::ranges::find_if(package.targets, [&](const PackageTarget& candidate) {
                        return candidate.name == target_name;
                    });
                    if (target != package.targets.end() && target->policy) {
                        context = target_name;
                        instance = find_configured_package_instance(instances, package.id, context);
                        if (!instance) {
                            return std::unexpected(
                                error_at(target->location, "configured instance is missing for target `" + target_name + "`")
                            );
                        }
                    }
                }

                ConfiguredRoute& route = route_for(*instance, std::move(context));
                if (std::ranges::find(route.request.targets, target_name) == route.request.targets.end())
                    route.request.targets.push_back(target_name);
            }

            if (routes.empty()) {
                return std::unexpected(error("CMake build request selects neither default nor explicit targets"));
            }
            return routes;
        }

        Result<bool> requires_install(
            const Graph& graph,
            const ExtensionRegistry& registry,
            const PackageNode& package,
            const ProductRealizationContext& realization,
            ConfigurationCache& cache
        ) {
            if (!cache.install_requirements) {
                std::vector<bool> requirements(graph.size(), false);
                for (const PackageNode& candidate: graph.nodes()) {
                    if (!has_build_semantics(candidate.kind) || candidate.resolver != "cmake")
                        continue;

                    auto options = read_cached_options(graph, registry, candidate, realization, nullptr, {}, cache);
                    if (!options)
                        return std::unexpected(options.error());

                    for (const PackageId dependency: candidate.dependencies) {
                        if (dependency_mode(**options, dependency) == DependencyMode::find_package)
                            requirements[dependency.index] = true;
                    }
                }
                cache.install_requirements = std::move(requirements);
            }
            return (*cache.install_requirements)[package.id.index];
        }

        struct SourceDependencyContext {
            const Graph& graph;
            const ExtensionRegistry& registry;
            const ProductRealizationContext& realization;
            std::vector<bool>& visited;
            std::vector<PackageId>& packages;
            ConfigurationCache& cache;
        };

        Result<void> collect_source_dependencies(const PackageId id, const bool include_associated, SourceDependencyContext& context) {
            if (context.visited[id.index])
                return {};

            context.visited[id.index] = true;

            const PackageNode& package = context.graph[id];
            auto options = read_cached_options(context.graph, context.registry, package, context.realization, nullptr, {}, context.cache);
            if (!options)
                return std::unexpected(options.error());

            for (const PackageId dependency: package.dependencies) {
                const PackageNode& target = context.graph[dependency];
                if (!has_build_semantics(target.kind) || target.resolver != "cmake")
                    continue;

                if (dependency_mode(**options, dependency) != DependencyMode::add_subdirectory)
                    continue;

                auto collected = collect_source_dependencies(dependency, false, context);
                if (!collected)
                    return std::unexpected(collected.error());
            }

            if (include_associated) {
                for (const PackageTargetDependencies& dependencies: package.target_dependencies) {
                    if (std::ranges::none_of((*options)->targets, [&](const detail::TargetOptions& target) {
                            return target.name == dependencies.target;
                        })) {
                        continue;
                    }
                    for (const PackageId dependency: dependencies.packages) {
                        const PackageNode& target = context.graph[dependency];
                        if (!has_build_semantics(target.kind) || target.resolver != "cmake")
                            continue;

                        auto collected = collect_source_dependencies(dependency, false, context);
                        if (!collected)
                            return std::unexpected(collected.error());
                    }
                }
            }

            context.packages.push_back(id);
            return {};
        }

        struct PackagePrefixContext {
            const Graph& graph;
            const ExtensionRegistry& registry;
            const BuildEnvironment& environment;
            std::span<const ConfiguredPackageInstance> instances;
            std::string_view configured_context;
            std::vector<bool>& visited;
            std::vector<bool>& added;
            std::vector<std::filesystem::path>& prefixes;
            ConfigurationCache& cache;
        };

        Result<void> collect_package_prefixes(const PackageId id, PackagePrefixContext& context) {
            if (context.visited[id.index])
                return {};

            context.visited[id.index] = true;

            const PackageNode& package = context.graph[id];
            auto options = read_cached_options(
                context.graph,
                context.registry,
                package,
                {context.environment.configuration.profile, host_target_os()},
                nullptr,
                {},
                context.cache
            );
            if (!options)
                return std::unexpected(options.error());

            for (const PackageId dependency: package.dependencies) {
                const PackageNode& target = context.graph[dependency];
                if (!has_build_semantics(target.kind) || target.resolver != "cmake")
                    continue;

                if (dependency_mode(**options, dependency) == DependencyMode::find_package && !context.added[dependency.index]) {
                    context.added[dependency.index] = true;
                    const ConfiguredPackageInstance* instance = find_configured_package_instance(
                        context.instances,
                        dependency,
                        context.configured_context
                    );
                    if (!instance) {
                        return std::unexpected(error("configured instance is missing for CMake dependency `" + target.name + "`"));
                    }
                    context.prefixes.push_back(artifact_directory(context.environment, *instance));
                }

                auto collected = collect_package_prefixes(dependency, context);
                if (!collected)
                    return std::unexpected(collected.error());
            }
            return {};
        }

        std::string join_prefixes(const std::vector<std::filesystem::path>& prefixes) {
            std::string result;
            for (const std::filesystem::path& prefix: prefixes) {
                if (!result.empty())
                    result += ';';

                result += prefix.string();
            }
            return result;
        }

        struct PreparedProject {
            std::filesystem::path source;
            std::filesystem::path cmakelists;
        };

        struct BuildContext {
            Options project;
            detail::BuildOptions build;
            std::optional<std::string> generator;
            BuildVariant variant;
            std::string configured_context;
            std::string configured_artifact;
            std::string configuration;
            std::filesystem::path directory;
            std::filesystem::path output;
            std::filesystem::path metadata;
        };

        std::filesystem::path product_metadata_directory(const BuildContext& context) {
            return context.directory / ".kaixa" / "products";
        }

        bool valid_cmake_variable(const std::string_view name) {
            if (name.empty() || (!std::isalpha(static_cast<unsigned char>(name.front())) && name.front() != '_'))
                return false;

            return std::ranges::all_of(name.substr(1), [](const unsigned char character) {
                return std::isalnum(character) || character == '_';
            });
        }

        Result<std::vector<TableEntry>> consumer_options(const PackageNode& package) {
            if (!package.descriptor)
                return std::vector<TableEntry>{};

            const Value* consumer = package.descriptor->find("consumer");
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
            if (!package.descriptor)
                return std::vector<std::filesystem::path>{};

            const Value* consumer = package.descriptor->find("consumer");
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
            if (!package.descriptor)
                return std::vector<std::pair<std::string, std::string>>{};

            const Value* consumer = package.descriptor->find("consumer");
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
            if (!package.descriptor)
                return std::vector<ConsumerTargetPatch>{};

            const Value* consumer = package.descriptor->find("consumer");
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
            const Value* declared = package.descriptor ? package.descriptor->find(key) : nullptr;
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

        struct DependencyIntegrationContext {
            const Graph& graph;
            const PackageNode& package;
            const BuildContext& build;
            const std::vector<bool>& normal_source_visited;
            std::span<const PackageId> source_packages;
            const std::vector<std::optional<PreparedProject>>& projects;
        };

        Result<std::string> source_only_interface_integration(const Graph& graph) {
            std::string result;
            for (const PackageNode& package: graph.nodes()) {
                if (package.kind != PackageKind::opaque || !package.descriptor)
                    continue;

                const Value* kind = package.descriptor->find("kind");
                const std::string* kind_name = kind ? kind->as_string() : nullptr;
                const Value* products = package.descriptor->find("products");
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

        Result<ProductKind> product_kind(const std::string_view type) {
            if (type == "EXECUTABLE")
                return ProductKind::executable;

            if (type == "STATIC_LIBRARY")
                return ProductKind::static_library;

            if (type == "SHARED_LIBRARY")
                return ProductKind::shared_library;

            if (type == "MODULE_LIBRARY")
                return ProductKind::module_library;

            if (type == "OBJECT_LIBRARY")
                return ProductKind::object_library;

            if (type == "INTERFACE_LIBRARY")
                return ProductKind::interface_library;

            if (type == "UTILITY")
                return ProductKind::utility;

            return std::unexpected(error("unsupported CMake target type `" + std::string(type) + "`"));
        }

        ProductPurpose product_purpose(const PackageNode& package, const std::string_view name) {
            if (!package.manifest)
                return ProductPurpose::primary;

            const auto target = std::ranges::find_if(package.targets, [&](const PackageTarget& candidate) {
                return candidate.name == name;
            });
            if (target == package.targets.end())
                return ProductPurpose::primary;

            switch (target->kind) {
            case PackageTargetKind::test: return ProductPurpose::test;
            case PackageTargetKind::example: return ProductPurpose::example;
            case PackageTargetKind::benchmark: return ProductPurpose::benchmark;
            }
            return ProductPurpose::primary;
        }

        Result<std::vector<BuildProduct>> read_products(const PackageNode& package, const BuildContext& context) {
            const std::filesystem::path directory = product_metadata_directory(context) / context.configuration;
            std::error_code failure;
            const bool exists = std::filesystem::exists(directory, failure);
            if (failure) {
                return std::unexpected(error("cannot inspect CMake products in `" + directory.string() + "`: " + failure.message()));
            }
            if (!exists) {
                return std::unexpected(
                    error("CMake target information is unavailable").add_note("run `kaixa generate` to configure the workspace")
                );
            }
            if (!std::filesystem::is_directory(directory, failure) || failure) {
                return std::unexpected(error("CMake product metadata is not a directory: " + directory.string()));
            }

            std::vector<BuildProduct> products;
            std::filesystem::directory_iterator entries(directory, failure);
            if (failure) {
                return std::unexpected(error("cannot list CMake products in `" + directory.string() + "`: " + failure.message()));
            }

            for (const std::filesystem::directory_entry& entry: entries) {
                if (entry.path().extension() != ".product")
                    continue;

                std::ifstream input(entry.path(), std::ios::binary);
                std::string name;
                std::string type;
                std::string artifact;
                if (!input || !std::getline(input, name) || !std::getline(input, type) || !std::getline(input, artifact)) {
                    return std::unexpected(error("cannot read CMake product metadata `" + entry.path().string() + "`"));
                }
                if (!name.empty() && name.back() == '\r')
                    name.pop_back();

                if (!type.empty() && type.back() == '\r')
                    type.pop_back();

                if (!artifact.empty() && artifact.back() == '\r')
                    artifact.pop_back();

                if (name.empty() || type.empty()) {
                    return std::unexpected(error("invalid CMake product metadata `" + entry.path().string() + "`"));
                }

                auto kind = product_kind(type);
                if (!kind)
                    return std::unexpected(std::move(kind).error().add_note("in `" + entry.path().string() + "`"));

                const ProductPurpose purpose = product_purpose(package, name);
                BuildProduct product{std::move(name), *kind, purpose, package.id, std::nullopt};
                if (!artifact.empty())
                    product.artifact = std::move(artifact);

                products.push_back(std::move(product));
            }

            std::ranges::sort(products, {}, &BuildProduct::name);
            return products;
        }

        void append_toml_array(std::string& output, const std::string_view name, const std::span<const std::string> values) {
            output += std::string(name) + " = [";
            for (std::size_t index = 0; index < values.size(); ++index) {
                if (index != 0)
                    output += ", ";

                output += toml_string(values[index]);
            }
            output += "]\n";
        }

        std::string variant_metadata(const BuildEnvironment& environment, const BuildContext& context) {
            std::string output = "# Generated by Kaixa.\n"
                                 "resolver = \"cmake\"\n"
                                 "label = "
                + toml_string(context.variant.label)
                + "\n"
                  "fingerprint = "
                + toml_string(context.variant.fingerprint)
                + "\n"
                  "build = "
                + toml_string(context.directory.generic_string())
                + "\n"
                  "output = "
                + toml_string(context.output.generic_string())
                + "\n"
                  "profile = "
                + toml_string(environment.configuration.profile)
                + "\n"
                  "source = "
                + toml_string(context.project.source.generic_string())
                + "\n"
                  "generation = "
                + toml_string(context.project.generation == GenerationMode::export_project ? "export" : "state")
                + "\n"
                  "configured-context = "
                + toml_string(context.configured_context)
                + "\n"
                  "configured-artifact = "
                + toml_string(context.configured_artifact)
                + "\n";
            append_toml_array(output, "configs", environment.configuration.selected);
            output += "\n[cmake]\n";
            if (context.generator)
                output += "generator = " + toml_string(*context.generator) + "\n";

            if (context.build.c_compiler) {
                output += "c-compiler = " + toml_string(*context.build.c_compiler) + "\n";
            }
            if (context.build.cxx_compiler) {
                output += "cxx-compiler = " + toml_string(*context.build.cxx_compiler) + "\n";
            }
            if (context.build.toolchain) {
                output += "toolchain = " + toml_string(context.build.toolchain->generic_string()) + "\n";
            }
            append_toml_array(output, "configure-arguments", context.build.configure_arguments);
            return output;
        }

        Result<BuildContext> prepare_build_context(
            const Graph& graph,
            const ExtensionRegistry& registry,
            const PackageNode& package,
            const BuildEnvironment& environment,
            const ConfiguredPackageInstance& instance,
            const std::string_view configured_context,
            ConfigurationCache& cache
        ) {
            auto cached_project = read_cached_options(
                graph,
                registry,
                package,
                {environment.configuration.profile, host_target_os()},
                &instance.policy,
                configured_context,
                cache
            );
            if (!cached_project)
                return std::unexpected(cached_project.error());

            Options project = **cached_project;

            const bool primary = configured_context == package.name + ":default";
            if (!primary && !project.targets.empty())
                project.generation = GenerationMode::state;

            const ResolverBuildConfiguration* configuration = environment.configuration.find("cmake");
            auto build = read_build_options(configuration && configuration->settings ? &*configuration->settings : nullptr);
            if (!build)
                return std::unexpected(build.error());

            if (configuration) {
                build->configure_arguments
                    .insert(build->configure_arguments.end(), configuration->arguments.begin(), configuration->arguments.end());
                for (const ResolverArgumentGroup& scoped: configuration->scoped_arguments) {
                    std::vector<std::string>* destination = nullptr;
                    if (scoped.scope == "configure")
                        destination = &build->configure_arguments;
                    else if (scoped.scope == "build")
                        destination = &build->build_arguments;
                    else if (scoped.scope == "install")
                        destination = &build->install_arguments;
                    else {
                        return std::unexpected(
                            error("unknown CMake argument scope `" + scoped.scope + "`").add_note("expected configure, build or install")
                        );
                    }

                    destination->insert(destination->end(), scoped.arguments.begin(), scoped.arguments.end());
                }
            }

            std::optional<std::string> generator = requested_generator(build->configure_arguments);
            if (!generator)
                generator = build->generator;

            BuildVariant variant = build_variant(environment, *build, build->configure_arguments, project, instance, primary);
            const std::filesystem::path directory = cmake_build_root(environment, variant.directory) / package.name;
            const std::filesystem::path output = environment.state_root / "build" / variant.directory;
            const std::filesystem::path metadata = graph.roots().size() == 1 && graph.is_root(package.id)
                ? directory.parent_path() / "variant.toml"
                : directory.parent_path() / ".variants" / (package.name + ".toml");
            return BuildContext{std::move(project),
                std::move(*build),
                std::move(generator),
                std::move(variant),
                std::string(configured_context),
                instance.artifact,
                configuration_name(environment.configuration.profile),
                directory,
                output,
                metadata};
        }

        Result<std::optional<std::string>> stored_fingerprint(const std::filesystem::path& metadata) {
            std::error_code failure;
            if (!std::filesystem::exists(metadata, failure)) {
                if (failure) {
                    return std::unexpected(
                        error("cannot inspect CMake variant metadata `" + metadata.string() + "`: " + failure.message())
                    );
                }

                return std::nullopt;
            }

            std::ifstream input(metadata, std::ios::binary);
            if (!input) {
                return std::unexpected(error("cannot read CMake variant metadata `" + metadata.string() + "`"));
            }

            std::string line;
            constexpr std::string_view prefix = "fingerprint = \"";
            while (std::getline(input, line)) {
                if (!line.starts_with(prefix) || !line.ends_with('"'))
                    continue;

                return line.substr(prefix.size(), line.size() - prefix.size() - 1);
            }
            return std::nullopt;
        }

        Result<bool> requires_reset(const BuildContext& context) {
            std::error_code failure;
            const bool exists = std::filesystem::exists(context.directory, failure);
            if (failure) {
                return std::unexpected(error("cannot inspect CMake build tree `" + context.directory.string() + "`: " + failure.message()));
            }
            if (!exists)
                return false;

            auto fingerprint = stored_fingerprint(context.metadata);
            if (!fingerprint)
                return std::unexpected(fingerprint.error());

            return !*fingerprint || **fingerprint != context.variant.fingerprint;
        }

        std::string reset_script(const BuildContext& context) {
            const std::filesystem::path query = detail::file_api_query(context.directory);
            return "file(REMOVE_RECURSE "
                + detail::syntax::path(context.directory)
                + ")\n"
                + "file(MAKE_DIRECTORY "
                + detail::syntax::path(query.parent_path())
                + ")\n"
                + "file(WRITE "
                + detail::syntax::path(query)
                + " \"\")\n";
        }

        struct ProjectPreparationContext {
            const Graph& graph;
            const ExtensionRegistry& registry;
            const BuildEnvironment& environment;
            std::string_view variant;
            std::span<const ConfiguredPackageInstance> instances;
            std::string_view configured_context;
            bool isolated = false;
            std::set<std::filesystem::path> generated_projects;
            BuildPlan& plan;
            ConfigurationCache& cache;
        };

        Result<PreparedProject> prepare_project(const PackageNode& package, ProjectPreparationContext& context) {
            const ConfiguredPackageInstance* instance = find_configured_package_instance(
                context.instances,
                package.id,
                context.configured_context
            );
            if (!instance) {
                const std::string default_context = package.name + ":default";
                instance = find_configured_package_instance(context.instances, package.id, default_context);
            }
            if (!instance)
                return std::unexpected(error("configured instance is missing for CMake package `" + package.name + "`"));

            auto cached_options = read_cached_options(
                context.graph,
                context.registry,
                package,
                {context.environment.configuration.profile, host_target_os()},
                &instance->policy,
                context.configured_context,
                context.cache
            );
            if (!cached_options)
                return std::unexpected(cached_options.error());

            Options options = **cached_options;

            if (context.isolated && !options.targets.empty())
                options.generation = GenerationMode::state;

            std::filesystem::path source = options.source;
            if (!options.targets.empty() && options.generation == GenerationMode::state) {
                source = context.environment.state_root / "generated" / "cmake" / context.variant / package.name / "project";
            }
            const std::filesystem::path project = source / "CMakeLists.txt";
            if (!options.targets.empty()) {
                if (options.generation == GenerationMode::export_project && std::filesystem::is_regular_file(project)) {
                    std::ifstream input(project, std::ios::binary);
                    std::string first_line;
                    if (!input || !std::getline(input, first_line)) {
                        return std::unexpected(error("cannot inspect existing `" + project.string() + "`"));
                    }
                    if (first_line.ends_with('\r'))
                        first_line.pop_back();

                    if (first_line != detail::generated_marker) {
                        SourceLocation location;
                        if (package.manifest)
                            location = package.manifest->location;

                        return std::unexpected(error_at(
                            std::move(location),
                            "refusing to overwrite `" + project.string() + "` because it was not generated by Kaixa"
                        ));
                    }
                }
                std::vector<detail::ProjectPackage> packages;
                if (options.generation == GenerationMode::export_project) {
                    for (const PackageNode& candidate: context.graph.nodes()) {
                        if (!has_build_semantics(candidate.kind)
                            || candidate.resolver != package.resolver
                            || candidate.directory != package.directory) {
                            continue;
                        }

                        const std::string candidate_context = candidate.name + ":default";
                        const ConfiguredPackageInstance* candidate_instance = find_configured_package_instance(
                            context.instances,
                            candidate.id,
                            candidate_context
                        );
                        if (!candidate_instance)
                            continue;

                        auto candidate_options = read_cached_options(
                            context.graph,
                            context.registry,
                            candidate,
                            {context.environment.configuration.profile, host_target_os()},
                            &candidate_instance->policy,
                            candidate_context,
                            context.cache
                        );
                        if (!candidate_options)
                            return std::unexpected(candidate_options.error());

                        if ((**candidate_options).generation == GenerationMode::export_project && !(**candidate_options).targets.empty()) {
                            packages.push_back({&candidate, *candidate_options});
                        }
                    }
                    std::ranges::sort(packages, {}, [](const detail::ProjectPackage& entry) { return entry.package->id; });
                } else {
                    packages.push_back({&package, &options});
                }

                if (context.generated_projects.insert(project).second)
                    context.plan.generate({project, detail::generate_project(packages)});
                return PreparedProject{std::move(source), project};
            }

            if (!std::filesystem::is_regular_file(project)) {
                SourceLocation location;
                if (package.manifest)
                    location = package.manifest->location;

                return std::unexpected(
                    error_at(std::move(location), "CMake package `" + package.name + "` has no `" + project.string() + "`")
                );
            }
            return PreparedProject{std::move(source), project};
        }

        struct ConfigureActionContext {
            const Graph& graph;
            const PackageNode& package;
            const BuildContext& build;
            const ConfiguredPackageInstance& instance;
            const std::optional<std::filesystem::path>& install;
            const std::filesystem::path& integration_file;
            const std::vector<std::filesystem::path>& prefixes;
            const std::vector<PackageId>& source_packages;
            const std::vector<std::optional<PreparedProject>>& projects;
            bool reset = false;
        };

        Result<Action> configure_action(const ConfigureActionContext& route) {
            const Graph& graph = route.graph;
            const PackageNode& package = route.package;
            const BuildContext& context = route.build;
            Action configure;
            configure.description = "configure " + package.name;
            configure.argv = {"cmake", "-S", route.projects[package.id.index]->source.string(), "-B", context.directory.string()};
            if (route.install)
                configure.argv.push_back("-DCMAKE_INSTALL_PREFIX=" + route.install->string());
            configure.argv.push_back("-DCMAKE_PROJECT_INCLUDE=" + route.integration_file.string());
            configure.inputs.push_back(context.metadata);
            configure.inputs.push_back(route.integration_file);
            configure.argv.push_back("-DKAIXA_CMAKE_PREFIX_PATH=" + join_prefixes(route.prefixes));
            if (graph.is_root(package.id)) {
                const std::string output = context.output.generic_string();
                if (context.project.runtime_output || context.project.library_output || context.project.archive_output)
                    configure.argv.push_back("-DKAIXA_OUTPUT_ROOT=" + output);

                configure.argv.push_back("-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=" + output + "/bin/$<0:>");
                configure.argv.push_back("-DCMAKE_LIBRARY_OUTPUT_DIRECTORY=" + output + "/lib/$<0:>");
                configure.argv.push_back("-DCMAKE_ARCHIVE_OUTPUT_DIRECTORY=" + output + "/lib/$<0:>");
            }
            if (!uses_multiple_configurations(context.generator))
                configure.argv.push_back("-DCMAKE_BUILD_TYPE=" + context.configuration);

            if (context.build.generator && !requested_generator(context.build.configure_arguments)) {
                configure.argv.push_back("-G");
                configure.argv.push_back(*context.build.generator);
            }
            if (context.build.c_compiler)
                configure.argv.push_back("-DCMAKE_C_COMPILER=" + *context.build.c_compiler);

            if (context.build.cxx_compiler)
                configure.argv.push_back("-DCMAKE_CXX_COMPILER=" + *context.build.cxx_compiler);

            if (context.build.toolchain) {
                if (!std::filesystem::is_regular_file(*context.build.toolchain)) {
                    return std::unexpected(error("CMake toolchain file does not exist: " + context.build.toolchain->string()));
                }
                configure.argv.push_back("-DCMAKE_TOOLCHAIN_FILE=" + context.build.toolchain->string());
            }
            configure.argv.insert(configure.argv.end(), context.build.configure_arguments.begin(), context.build.configure_arguments.end());
            configure.working_directory = package.directory;
            configure.package = package.id;
            configure.configured_artifact = route.instance.artifact;
            configure.stage = ActionStage::synchronize;
            for (const PackageId id: route.source_packages)
                configure.inputs.push_back(route.projects[id.index]->cmakelists);

            configure.outputs.push_back(context.directory / "CMakeCache.txt");
            auto checked_state = detail::configuration_state(context.directory, configure.inputs);
            configure.checked_state = route.reset ? ActionState::required : (checked_state ? *checked_state : ActionState::unknown);
            return configure;
        }

        Action build_action(
            const PackageNode& package,
            const BuildContext& context,
            const ConfiguredPackageInstance& instance,
            const BuildRequest& request,
            const bool installing,
            const bool selected
        ) {
            Action build;
            build.description = selected ? "build selected targets for " + package.name : "build " + package.name;
            build.argv = {"cmake", "--build", context.directory.string(), "--config", context.configuration};
            build.working_directory = package.directory;
            build.inputs.push_back(context.directory / "CMakeCache.txt");
            build.outputs.push_back(context.directory);
            build.package = package.id;
            build.configured_artifact = instance.artifact;
            if (selected) {
                build.argv.push_back("--target");
                build.argv.insert(build.argv.end(), request.targets.begin(), request.targets.end());
            }
            if (request.jobs) {
                build.argv.push_back("--parallel");
                build.argv.push_back(std::to_string(*request.jobs));
            }
            build.argv.insert(build.argv.end(), context.build.build_arguments.begin(), context.build.build_arguments.end());
            if (installing)
                build.stage = ActionStage::synchronize;

            return build;
        }

        Action install_action(
            const PackageNode& package,
            const BuildContext& context,
            const ConfiguredPackageInstance& instance,
            const std::filesystem::path& destination
        ) {
            Action action;
            action.description = "install " + package.name;
            action.argv = {"cmake",
                "--install",
                context.directory.string(),
                "--config",
                context.configuration,
                "--prefix",
                destination.string()};
            action.argv.insert(action.argv.end(), context.build.install_arguments.begin(), context.build.install_arguments.end());
            action.working_directory = package.directory;
            action.inputs.push_back(context.directory);
            action.outputs.push_back(destination);
            action.package = package.id;
            action.configured_artifact = instance.artifact;
            action.stage = ActionStage::synchronize;
            return action;
        }

        struct RouteDependencies {
            std::vector<bool> normal_source_visited;
            std::vector<PackageId> source_packages;
            std::vector<std::optional<PreparedProject>> projects;
        };

        Result<RouteDependencies> prepare_route_dependencies(
            const ConfiguredRoute& route,
            RoutePlanningContext& planning,
            const BuildContext& build,
            const ProductRealizationContext& realization
        ) {
            RouteDependencies result;
            result.normal_source_visited.resize(planning.graph.size(), false);
            std::vector<PackageId> normal_source_packages;
            SourceDependencyContext normal_source_context{planning.graph,
                planning.registry,
                realization,
                result.normal_source_visited,
                normal_source_packages,
                planning.cache};
            auto normal_sources = collect_source_dependencies(planning.package.id, false, normal_source_context);
            if (!normal_sources)
                return std::unexpected(normal_sources.error());

            std::vector<bool> source_visited(planning.graph.size(), false);
            SourceDependencyContext source_context{planning.graph,
                planning.registry,
                realization,
                source_visited,
                result.source_packages,
                planning.cache};
            auto sources = collect_source_dependencies(planning.package.id, true, source_context);
            if (!sources)
                return std::unexpected(sources.error());

            result.projects.resize(planning.graph.size());
            ProjectPreparationContext project_context{planning.graph,
                planning.registry,
                planning.environment,
                build.variant.directory,
                planning.instances,
                route.context,
                route.context != planning.package.name + ":default",
                {},
                planning.plan,
                planning.cache};
            for (const PackageId id: result.source_packages) {
                auto project = prepare_project(planning.graph[id], project_context);
                if (!project)
                    return std::unexpected(project.error());

                result.projects[id.index] = std::move(*project);
            }
            return result;
        }

        Result<std::filesystem::path> generate_route_dependencies(
            RoutePlanningContext& planning,
            const BuildContext& build,
            const RouteDependencies& dependencies
        ) {
            std::filesystem::path integration_file = planning.environment.state_root
                / "generated"
                / "cmake"
                / build.variant.directory
                / planning.package.name
                / "dependencies.cmake";
            auto integration = dependency_integration(
                {planning.graph,
                    planning.package,
                    build,
                    dependencies.normal_source_visited,
                    dependencies.source_packages,
                    dependencies.projects}
            );
            if (!integration)
                return std::unexpected(integration.error());

            planning.plan.generate({integration_file, std::move(*integration)});
            if (build.project.generation == GenerationMode::export_project) {
                std::vector<detail::PortableProject> portable_projects;
                portable_projects.reserve(dependencies.source_packages.size());
                for (const PackageId id: dependencies.source_packages) {
                    portable_projects.push_back(
                        {id, dependencies.projects[id.index]->source, !dependencies.normal_source_visited[id.index]}
                    );
                }

                auto portable = detail::generate_portable_dependencies(planning.graph, planning.package, build.project, portable_projects);
                if (!portable)
                    return std::unexpected(portable.error());

                planning.plan.generate({build.project.source / "KaixaDependencies.cmake", std::move(*portable)});
            }
            planning.plan.generate({detail::file_api_query(build.directory), {}});
            return integration_file;
        }

        void append_reset_action(const ConfiguredRoute& route, RoutePlanningContext& planning, const BuildContext& build) {
            const std::filesystem::path script = planning.environment.state_root
                / "generated"
                / "cmake"
                / build.variant.directory
                / planning.package.name
                / "reset.cmake";
            planning.plan.generate({script, reset_script(build)});

            Action action;
            action.description = "reset " + planning.package.name;
            action.argv = {"cmake", "-P", script.string()};
            action.working_directory = planning.package.directory;
            action.inputs.push_back(script);
            action.checked_state = ActionState::required;
            action.package = planning.package.id;
            action.configured_artifact = route.instance->artifact;
            action.stage = ActionStage::synchronize;
            planning.plan.add(std::move(action));
        }

        Result<void> append_route_actions(
            const ConfiguredRoute& route,
            RoutePlanningContext& planning,
            const BuildContext& build,
            const RouteDependencies& dependencies,
            const std::filesystem::path& integration_file,
            const std::optional<std::filesystem::path>& install,
            const bool reset
        ) {
            std::vector<bool> prefix_visited(planning.graph.size(), false);
            std::vector<bool> prefix_added(planning.graph.size(), false);
            std::vector<std::filesystem::path> prefixes;
            PackagePrefixContext prefix_context{planning.graph,
                planning.registry,
                planning.environment,
                planning.instances,
                route.context,
                prefix_visited,
                prefix_added,
                prefixes,
                planning.cache};
            auto prefix_result = collect_package_prefixes(planning.package.id, prefix_context);
            if (!prefix_result)
                return std::unexpected(prefix_result.error());

            auto configure = configure_action(
                {planning.graph,
                    planning.package,
                    build,
                    *route.instance,
                    install,
                    integration_file,
                    prefixes,
                    dependencies.source_packages,
                    dependencies.projects,
                    reset}
            );
            if (!configure)
                return std::unexpected(configure.error());

            planning.plan.add(std::move(*configure));
            if (route.request.build_default) {
                planning.plan.add(build_action(planning.package, build, *route.instance, route.request, install.has_value(), false));
            }
            if (!route.request.targets.empty()) {
                planning.plan.add(build_action(planning.package, build, *route.instance, route.request, install.has_value(), true));
            }
            if (!route.request.build_default && route.request.targets.empty()) {
                return std::unexpected(error("CMake build request selects neither default nor explicit targets"));
            }
            if (install)
                planning.plan.add(install_action(planning.package, build, *route.instance, *install));

            return {};
        }

        class ResolverImpl final : public Resolver {
            class Session final : public ResolverSession {
            public:
                explicit Session(const std::size_t package_count)
                    : configuration(package_count) {}

                ConfigurationCache configuration;
            };

        public:
            [[nodiscard]] ResolverInfo info() const override { return {"cmake", "generates, adopts and composes CMake projects"}; }

            [[nodiscard]] std::unique_ptr<ResolverSession> start_session(const Graph& graph) const override {
                return std::make_unique<Session>(graph.size());
            }

            [[nodiscard]] Result<void> plan(
                const Graph& graph,
                const ExtensionRegistry& registry,
                const PackageNode& package,
                const BuildEnvironment& environment,
                const std::span<const ConfiguredPackageInstance> instances,
                const BuildRequest& request,
                BuildPlan& plan,
                ResolverSession& resolver_session
            ) const override {
                ConfigurationCache& cache = static_cast<Session&>(resolver_session).configuration;
                std::vector<ConfiguredRoute> routes;
                if (graph.is_root(package.id)) {
                    auto configured = configured_routes(package, request, instances);
                    if (!configured)
                        return std::unexpected(configured.error());

                    routes = std::move(*configured);
                } else {
                    for (const ConfiguredPackageInstance& instance: instances) {
                        if (instance.package != package.id || instance.contexts.empty())
                            continue;

                        const std::string default_context = package.name + ":default";
                        const bool is_default = std::ranges::find(instance.contexts, default_context) != instance.contexts.end();
                        BuildRequest routed_request;
                        routed_request.jobs = request.jobs;
                        routes.push_back({is_default ? default_context : instance.contexts.front(), &instance, std::move(routed_request)});
                    }
                }

                RoutePlanningContext planning{graph, registry, package, environment, instances, plan, cache};
                for (const ConfiguredRoute& route: routes) {
                    auto planned = plan_route(route, planning);
                    if (!planned)
                        return std::unexpected(planned.error());
                }
                return {};
            }

        private:
            [[nodiscard]] Result<void> plan_route(const ConfiguredRoute& route, RoutePlanningContext& planning) const {
                const ProductRealizationContext realization{planning.environment.configuration.profile, host_target_os()};
                auto
                    dependency_install = requires_install(planning.graph, planning.registry, planning.package, realization, planning.cache);
                if (!dependency_install)
                    return std::unexpected(dependency_install.error());

                const std::optional<std::filesystem::path>
                    install = install_destination(route.request, *dependency_install, planning.environment, *route.instance);
                if (!planning.graph.is_root(planning.package.id) && !install)
                    return {};

                auto build = prepare_build_context(
                    planning.graph,
                    planning.registry,
                    planning.package,
                    planning.environment,
                    *route.instance,
                    route.context,
                    planning.cache
                );
                if (!build)
                    return std::unexpected(build.error());

                auto reset = requires_reset(*build);
                if (!reset)
                    return std::unexpected(reset.error());

                planning.plan.generate({build->metadata, variant_metadata(planning.environment, *build)});
                if (planning.graph.is_root(planning.package.id)) {
                    planning.plan.output({planning.package.id, "cmake", build->output, build->directory, route.instance->artifact});
                }

                auto dependencies = prepare_route_dependencies(route, planning, *build, realization);
                if (!dependencies)
                    return std::unexpected(dependencies.error());

                auto integration_file = generate_route_dependencies(planning, *build, *dependencies);
                if (!integration_file)
                    return std::unexpected(integration_file.error());

                if (*reset)
                    append_reset_action(route, planning, *build);

                return append_route_actions(route, planning, *build, *dependencies, *integration_file, install, *reset);
            }

        public:
            [[nodiscard]] Result<void> plan_tests(
                const Graph& graph,
                const ExtensionRegistry& registry,
                const PackageNode& package,
                const BuildEnvironment& environment,
                const std::span<const ConfiguredPackageInstance> instances,
                const TestRequest& request,
                BuildPlan& plan,
                ResolverSession& resolver_session
            ) const override {
                ConfigurationCache& cache = static_cast<Session&>(resolver_session).configuration;
                const std::string configured_context = package.name + ":default";
                const ConfiguredPackageInstance* instance = find_configured_package_instance(instances, package.id, configured_context);
                if (!instance)
                    return std::unexpected(error("configured default instance is missing for package `" + package.name + "`"));

                auto default_build_context = prepare_build_context(
                    graph,
                    registry,
                    package,
                    environment,
                    *instance,
                    configured_context,
                    cache
                );
                if (!default_build_context)
                    return std::unexpected(default_build_context.error());

                std::vector<std::string> test_targets;
                if (request.target) {
                    test_targets.push_back(*request.target);
                } else {
                    for (const detail::TestOptions& test: default_build_context->project.tests) {
                        if ((request.purpose == ProductPurpose::benchmark) != (test.adapter.purpose == TestAdapterPurpose::benchmark)) {
                            continue;
                        }
                        if (std::ranges::find(test_targets, test.target) == test_targets.end())
                            test_targets.push_back(test.target);
                    }
                }
                if (test_targets.empty()) {
                    return detail::plan_tests(
                        default_build_context->project,
                        package,
                        {default_build_context->directory, default_build_context->configuration, {}, instance->artifact},
                        request,
                        plan
                    );
                }

                BuildRequest selection;
                selection.targets = test_targets;
                selection.build_default = false;
                auto routes = configured_routes(package, selection, instances);
                if (!routes)
                    return std::unexpected(routes.error());

                RoutePlanningContext planning{graph, registry, package, environment, instances, plan, cache};
                for (const ConfiguredRoute& route: *routes) {
                    const bool has_build_action = std::ranges::any_of(plan.actions(), [&](const Action& action) {
                        return action.package == package.id
                            && action.configured_artifact == route.instance->artifact
                            && action.stage == ActionStage::build;
                    });
                    if (!has_build_action) {
                        ConfiguredRoute build_route = route;
                        build_route.request.targets.clear();
                        build_route.request.build_default = true;
                        auto planned = plan_route(build_route, planning);
                        if (!planned)
                            return std::unexpected(planned.error());
                    }

                    auto context = prepare_build_context(graph, registry, package, environment, *route.instance, route.context, cache);
                    if (!context)
                        return std::unexpected(context.error());

                    auto planned_tests = detail::plan_tests(
                        context->project,
                        package,
                        {context->directory, context->configuration, route.request.targets, route.instance->artifact},
                        request,
                        plan
                    );
                    if (!planned_tests)
                        return std::unexpected(planned_tests.error());
                }
                return {};
            }

            [[nodiscard]] Result<std::vector<BuildProduct>> products(
                const Graph& graph,
                const ExtensionRegistry& registry,
                const PackageNode& package,
                const BuildEnvironment& environment,
                const std::span<const ConfiguredPackageInstance> instances,
                ResolverSession& resolver_session
            ) const override {
                ConfigurationCache& cache = static_cast<Session&>(resolver_session).configuration;
                const std::string configured_context = package.name + ":default";
                const ConfiguredPackageInstance* instance = find_configured_package_instance(instances, package.id, configured_context);
                if (!instance)
                    return std::unexpected(error("configured default instance is missing for package `" + package.name + "`"));

                auto context = prepare_build_context(graph, registry, package, environment, *instance, configured_context, cache);
                if (!context)
                    return std::unexpected(context.error());

                return read_products(package, *context);
            }

            [[nodiscard]] Result<void> plan_clean(
                const Graph& graph,
                const ExtensionRegistry& registry,
                const PackageNode& package,
                const BuildEnvironment& environment,
                const std::span<const ConfiguredPackageInstance> instances,
                const CleanRequest& request,
                CleanPlan& plan,
                ResolverSession& resolver_session
            ) const override {
                ConfigurationCache& cache = static_cast<Session&>(resolver_session).configuration;
                const std::string configured_context = package.name + ":default";
                const ConfiguredPackageInstance* default_instance = find_configured_package_instance(
                    instances,
                    package.id,
                    configured_context
                );
                if (!default_instance)
                    return std::unexpected(error("configured default instance is missing for package `" + package.name + "`"));

                for (const ConfiguredPackageInstance& instance: instances) {
                    if (instance.package != package.id || instance.contexts.empty())
                        continue;

                    const bool is_default = instance.artifact == default_instance->artifact;
                    const std::string_view context_name = is_default ? configured_context : instance.contexts.front();
                    auto context = prepare_build_context(graph, registry, package, environment, instance, context_name, cache);
                    if (!context)
                        return std::unexpected(context.error());

                    plan.add(context->output);
                    plan.add(cmake_build_root(environment, context->variant.directory));
                    plan.add(artifact_directory(environment, instance));
                }
                if (request.generated_files) {
                    auto context = prepare_build_context(
                        graph,
                        registry,
                        package,
                        environment,
                        *default_instance,
                        configured_context,
                        cache
                    );
                    if (!context)
                        return std::unexpected(context.error());

                    if (!context->project.targets.empty() && context->project.generation == GenerationMode::export_project) {
                        plan.generated_file({context->project.source / "CMakeLists.txt", std::string(detail::generated_marker)});
                        plan.generated_file({context->project.source / "KaixaDependencies.cmake", std::string(detail::generated_marker)});
                    }
                }
                return {};
            }
        };
    }

    std::unique_ptr<Resolver> make_resolver() {
        return std::make_unique<ResolverImpl>();
    }
}
