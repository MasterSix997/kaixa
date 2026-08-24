#include <kaixa/plugin/cmake/resolver.hpp>

#include <kaixa/config/value_operations.hpp>
#include <kaixa/extension/registry.hpp>
#include <kaixa/foundation/process.hpp>

#include "configuration.hpp"
#include "file_api.hpp"
#include "testing.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kaixa::plugin::cmake {
    namespace {
        using detail::dependency_mode;
        using detail::DependencyMode;
        using detail::GenerationMode;
        using detail::Options;
        using detail::read_build_options;
        using detail::read_options;

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
            absorb(project.generation == GenerationMode::source ? "source" : "state");
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
            const ProductRealizationContext& realization
        ) {
            for (const PackageNode& candidate: graph.nodes()) {
                if (candidate.kind != PackageKind::managed || candidate.resolver != "cmake")
                    continue;

                const auto dependency = std::ranges::find(candidate.dependencies, package.id);
                if (dependency == candidate.dependencies.end())
                    continue;

                auto options = read_options(graph, registry, candidate, realization);
                if (!options)
                    return std::unexpected(options.error());

                if (dependency_mode(*options, package.id) == DependencyMode::find_package)
                    return true;
            }
            return false;
        }

        Result<void> collect_source_dependencies(
            const Graph& graph,
            const ExtensionRegistry& registry,
            const PackageId id,
            const bool include_associated,
            const ProductRealizationContext& realization,
            std::vector<bool>& visited,
            std::vector<PackageId>& packages
        ) {
            if (visited[id.index])
                return {};

            visited[id.index] = true;

            const PackageNode& package = graph[id];
            auto options = read_options(graph, registry, package, realization);
            if (!options)
                return std::unexpected(options.error());

            for (const PackageId dependency: package.dependencies) {
                const PackageNode& target = graph[dependency];
                if (target.kind != PackageKind::managed || target.resolver != "cmake")
                    continue;

                if (dependency_mode(*options, dependency) != DependencyMode::add_subdirectory)
                    continue;

                auto collected = collect_source_dependencies(graph, registry, dependency, false, realization, visited, packages);
                if (!collected)
                    return std::unexpected(collected.error());
            }

            if (include_associated) {
                for (const PackageTargetDependencies& dependencies: package.target_dependencies) {
                    if (std::ranges::none_of(options->targets, [&](const detail::TargetOptions& target) {
                            return target.name == dependencies.target;
                        })) {
                        continue;
                    }
                    for (const PackageId dependency: dependencies.packages) {
                        const PackageNode& target = graph[dependency];
                        if (target.kind != PackageKind::managed || target.resolver != "cmake")
                            continue;

                        auto collected = collect_source_dependencies(graph, registry, dependency, false, realization, visited, packages);
                        if (!collected)
                            return std::unexpected(collected.error());
                    }
                }
            }

            packages.push_back(id);
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
        };

        Result<void> collect_package_prefixes(const PackageId id, PackagePrefixContext& context) {
            if (context.visited[id.index])
                return {};

            context.visited[id.index] = true;

            const PackageNode& package = context.graph[id];
            auto options = read_options(
                context.graph,
                context.registry,
                package,
                {context.environment.configuration.profile, host_target_os()}
            );
            if (!options)
                return std::unexpected(options.error());

            for (const PackageId dependency: package.dependencies) {
                const PackageNode& target = context.graph[dependency];
                if (target.kind != PackageKind::managed || target.resolver != "cmake")
                    continue;

                if (dependency_mode(*options, dependency) == DependencyMode::find_package && !context.added[dependency.index]) {
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

        std::string cmake_quote(const std::filesystem::path& path) {
            const std::string value = path.generic_string();
            std::string equals;
            while (value.contains("]" + equals + "]"))
                equals += '=';

            return "[" + equals + "[" + value + "]" + equals + "]";
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

        std::string product_integration(const BuildContext& context) {
            const std::filesystem::path metadata = product_metadata_directory(context);
            const std::filesystem::path dependencies = context.directory / "_dependencies";
            return "  set(_kaixa_product_directory "
                + cmake_quote(metadata)
                + ")\n"
                  "  set(_kaixa_dependency_binary "
                + cmake_quote(dependencies)
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
                + toml_string(context.project.generation == GenerationMode::source ? "source" : "state")
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
            const std::string_view configured_context
        ) {
            auto project = read_options(
                graph,
                registry,
                package,
                {environment.configuration.profile, host_target_os()},
                &instance.policy,
                configured_context
            );
            if (!project)
                return std::unexpected(project.error());

            const bool primary = configured_context == package.name + ":default";
            if (!primary && !project->targets.empty())
                project->generation = GenerationMode::state;

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

            BuildVariant variant = build_variant(environment, *build, build->configure_arguments, *project, instance, primary);
            const std::filesystem::path directory = cmake_build_root(environment, variant.directory) / package.name;
            const std::filesystem::path output = environment.state_root / "build" / variant.directory;
            const std::filesystem::path metadata = graph.roots().size() == 1 && graph.is_root(package.id)
                ? directory.parent_path() / "variant.toml"
                : directory.parent_path() / ".variants" / (package.name + ".toml");
            return BuildContext{std::move(*project),
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
                + cmake_quote(context.directory)
                + ")\n"
                + "file(MAKE_DIRECTORY "
                + cmake_quote(query.parent_path())
                + ")\n"
                + "file(WRITE "
                + cmake_quote(query)
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
            BuildPlan& plan;
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

            auto options = read_options(
                context.graph,
                context.registry,
                package,
                {context.environment.configuration.profile, host_target_os()},
                &instance->policy,
                context.configured_context
            );
            if (!options)
                return std::unexpected(options.error());

            if (context.isolated && !options->targets.empty())
                options->generation = GenerationMode::state;

            std::filesystem::path source = options->source;
            if (!options->targets.empty() && options->generation == GenerationMode::state) {
                source = context.environment.state_root / "generated" / "cmake" / context.variant / package.name / "project";
            }
            const std::filesystem::path project = source / "CMakeLists.txt";
            if (!options->targets.empty()) {
                if (options->generation == GenerationMode::source && std::filesystem::is_regular_file(project)) {
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
                context.plan.generate({project, detail::generate_project(package, *options)});
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

        class ResolverImpl final : public Resolver {
        public:
            [[nodiscard]] ResolverInfo info() const override { return {"cmake", "generates, adopts and composes CMake projects"}; }

            [[nodiscard]] Result<void> plan(
                const Graph& graph,
                const ExtensionRegistry& registry,
                const PackageNode& package,
                const BuildEnvironment& environment,
                const std::span<const ConfiguredPackageInstance> instances,
                const BuildRequest& request,
                BuildPlan& plan
            ) const override {
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

                for (const ConfiguredRoute& route: routes) {
                    auto planned = plan_route(graph, registry, package, environment, instances, route, plan);
                    if (!planned)
                        return std::unexpected(planned.error());
                }
                return {};
            }

        private:
            [[nodiscard]] Result<void> plan_route(
                const Graph& graph,
                const ExtensionRegistry& registry,
                const PackageNode& package,
                const BuildEnvironment& environment,
                const std::span<const ConfiguredPackageInstance> instances,
                const ConfiguredRoute& route,
                BuildPlan& plan
            ) const {
                const BuildRequest& request = route.request;
                const ProductRealizationContext realization{environment.configuration.profile, host_target_os()};
                auto dependency_install = requires_install(graph, registry, package, realization);
                if (!dependency_install)
                    return std::unexpected(dependency_install.error());

                const std::optional<std::filesystem::path>
                    install = install_destination(request, *dependency_install, environment, *route.instance);
                if (!graph.is_root(package.id) && !install)
                    return {};

                auto context = prepare_build_context(graph, registry, package, environment, *route.instance, route.context);
                if (!context)
                    return std::unexpected(context.error());

                auto reset = requires_reset(*context);
                if (!reset)
                    return std::unexpected(reset.error());

                plan.generate({context->metadata, variant_metadata(environment, *context)});
                if (graph.is_root(package.id)) {
                    plan.output({package.id, "cmake", context->output, context->directory, route.instance->artifact});
                }

                std::vector<bool> normal_source_visited(graph.size(), false);
                std::vector<PackageId> normal_source_packages;
                auto normal_source_result = collect_source_dependencies(
                    graph,
                    registry,
                    package.id,
                    false,
                    realization,
                    normal_source_visited,
                    normal_source_packages
                );
                if (!normal_source_result)
                    return std::unexpected(normal_source_result.error());

                std::vector<bool> source_visited(graph.size(), false);
                std::vector<PackageId> source_packages;
                auto source_result = collect_source_dependencies(
                    graph,
                    registry,
                    package.id,
                    true,
                    realization,
                    source_visited,
                    source_packages
                );
                if (!source_result)
                    return std::unexpected(source_result.error());

                std::vector<std::optional<PreparedProject>> projects(graph.size());
                ProjectPreparationContext project_context{graph,
                    registry,
                    environment,
                    context->variant.directory,
                    instances,
                    route.context,
                    route.context != package.name + ":default",
                    plan};
                for (const PackageId id: source_packages) {
                    auto project = prepare_project(graph[id], project_context);
                    if (!project)
                        return std::unexpected(project.error());

                    projects[id.index] = std::move(*project);
                }

                const std::filesystem::path integration_file = environment.state_root
                    / "generated"
                    / "cmake"
                    / context->variant.directory
                    / package.name
                    / "dependencies.cmake";

                std::string integration = "# Generated by Kaixa.\n"
                                          "if(KAIXA_CMAKE_PREFIX_PATH)\n"
                                          "  list(PREPEND CMAKE_PREFIX_PATH ${KAIXA_CMAKE_PREFIX_PATH})\n"
                                          "endif()\n"
                                          "if(NOT KAIXA_CMAKE_DEPENDENCIES_INCLUDED)\n"
                                          "  set(KAIXA_CMAKE_DEPENDENCIES_INCLUDED TRUE)\n";
                integration += product_integration(*context);
                for (const PackageId id: source_packages) {
                    if (id == package.id)
                        continue;

                    const PackageNode& dependency = graph[id];

                    integration += "  add_subdirectory("
                        + cmake_quote(projects[id.index]->source)
                        + " "
                        + cmake_quote(context->directory / "_dependencies" / dependency.name);
                    if (!normal_source_visited[id.index])
                        integration += " EXCLUDE_FROM_ALL";

                    integration += ")\n";
                }
                integration += "endif()\n";
                plan.generate({integration_file, std::move(integration)});
                plan.generate({detail::file_api_query(context->directory), {}});

                if (*reset) {
                    const std::filesystem::path script = environment.state_root
                        / "generated"
                        / "cmake"
                        / context->variant.directory
                        / package.name
                        / "reset.cmake";
                    plan.generate({script, reset_script(*context)});

                    Action reset_action;
                    reset_action.description = "reset " + package.name;
                    reset_action.argv = {"cmake", "-P", script.string()};
                    reset_action.working_directory = package.directory;
                    reset_action.inputs.push_back(script);
                    reset_action.checked_state = ActionState::required;
                    reset_action.package = package.id;
                    reset_action.configured_artifact = route.instance->artifact;
                    reset_action.stage = ActionStage::synchronize;
                    plan.add(std::move(reset_action));
                }

                std::vector<bool> prefix_visited(graph.size(), false);
                std::vector<bool> prefix_added(graph.size(), false);
                std::vector<std::filesystem::path> prefixes;
                PackagePrefixContext
                    prefix_context{graph, registry, environment, instances, route.context, prefix_visited, prefix_added, prefixes};
                auto prefix_result = collect_package_prefixes(package.id, prefix_context);
                if (!prefix_result)
                    return std::unexpected(prefix_result.error());

                auto configure = configure_action(
                    {graph, package, *context, *route.instance, install, integration_file, prefixes, source_packages, projects, *reset}
                );
                if (!configure)
                    return std::unexpected(configure.error());

                plan.add(std::move(*configure));

                if (request.build_default)
                    plan.add(build_action(package, *context, *route.instance, request, install.has_value(), false));

                if (!request.targets.empty())
                    plan.add(build_action(package, *context, *route.instance, request, install.has_value(), true));

                if (!request.build_default && request.targets.empty()) {
                    return std::unexpected(error("CMake build request selects neither default nor explicit targets"));
                }

                if (install) {
                    plan.add(install_action(package, *context, *route.instance, *install));
                }
                return {};
            }

        public:
            [[nodiscard]] Result<void> plan_tests(
                const Graph& graph,
                const ExtensionRegistry& registry,
                const PackageNode& package,
                const BuildEnvironment& environment,
                const std::span<const ConfiguredPackageInstance> instances,
                const TestRequest& request,
                BuildPlan& plan
            ) const override {
                const std::string configured_context = package.name + ":default";
                const ConfiguredPackageInstance* instance = find_configured_package_instance(instances, package.id, configured_context);
                if (!instance)
                    return std::unexpected(error("configured default instance is missing for package `" + package.name + "`"));

                auto default_build_context = prepare_build_context(graph, registry, package, environment, *instance, configured_context);
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
                        auto planned = plan_route(graph, registry, package, environment, instances, build_route, plan);
                        if (!planned)
                            return std::unexpected(planned.error());
                    }

                    auto context = prepare_build_context(graph, registry, package, environment, *route.instance, route.context);
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
                const std::span<const ConfiguredPackageInstance> instances
            ) const override {
                const std::string configured_context = package.name + ":default";
                const ConfiguredPackageInstance* instance = find_configured_package_instance(instances, package.id, configured_context);
                if (!instance)
                    return std::unexpected(error("configured default instance is missing for package `" + package.name + "`"));

                auto context = prepare_build_context(graph, registry, package, environment, *instance, configured_context);
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
                CleanPlan& plan
            ) const override {
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
                    auto context = prepare_build_context(graph, registry, package, environment, instance, context_name);
                    if (!context)
                        return std::unexpected(context.error());

                    plan.add(context->output);
                    plan.add(cmake_build_root(environment, context->variant.directory));
                    plan.add(artifact_directory(environment, instance));
                }
                if (request.generated_files) {
                    auto context = prepare_build_context(graph, registry, package, environment, *default_instance, configured_context);
                    if (!context)
                        return std::unexpected(context.error());

                    if (!context->project.targets.empty() && context->project.generation == GenerationMode::source) {
                        plan.generated_file({context->project.source / "CMakeLists.txt", std::string(detail::generated_marker)});
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
