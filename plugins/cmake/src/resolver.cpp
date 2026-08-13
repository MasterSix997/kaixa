#include <kaixa/plugin/cmake/resolver.hpp>

#include <kaixa/foundation/process.hpp>

#include "configuration.hpp"
#include "file_api.hpp"
#include "testing.hpp"

#include <algorithm>
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
        using detail::DependencyMode;
        using detail::GenerationMode;
        using detail::Options;
        using detail::dependency_mode;
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
                if ((argument == "-G" || argument == "--generator")
                    && index + 1 < arguments.size()) {
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
                return generator.contains("Visual Studio")
                    || generator.contains("Xcode")
                    || generator.contains("Multi-Config");
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

            for (char& character: label) {
                const bool valid = (character >= 'a' && character <= 'z')
                    || (character >= 'A' && character <= 'Z')
                    || (character >= '0' && character <= '9')
                    || character == '-' || character == '_' || character == '+';
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
            const Options& project
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
            return {label, fingerprint, label};
        }

        std::filesystem::path cmake_build_root(const BuildEnvironment& environment, const std::string_view variant) {
            return environment.state_root / "build" / "cmake" / variant;
        }

        std::filesystem::path artifact_directory(
            const BuildEnvironment& environment,
            const PackageNode& package,
            const std::string_view variant
        ) {
            return environment.state_root / "cache" / "cmake" / variant / package.name;
        }

        Result<bool> requires_install(const Graph& graph, const PackageNode& package) {
            for (const PackageNode& candidate: graph.nodes()) {
                if (candidate.kind != PackageKind::managed || candidate.resolver != "cmake")
                    continue;

                const auto dependency = std::ranges::find(
                    candidate.dependencies,
                    package.id
                );
                if (dependency == candidate.dependencies.end())
                    continue;

                auto options = read_options(graph, candidate);
                if (!options)
                    return std::unexpected(options.error());
                if (dependency_mode(*options, package.id) == DependencyMode::find_package)
                    return true;
            }
            return false;
        }

        Result<void> collect_source_dependencies(
            const Graph& graph,
            const PackageId id,
            std::vector<bool>& visited,
            std::vector<PackageId>& packages
        ) {
            if (visited[id.index])
                return {};
            visited[id.index] = true;

            const PackageNode& package = graph[id];
            auto options = read_options(graph, package);
            if (!options)
                return std::unexpected(options.error());

            for (const PackageId dependency: package.dependencies) {
                const PackageNode& target = graph[dependency];
                if (target.kind != PackageKind::managed || target.resolver != "cmake")
                    continue;
                if (dependency_mode(*options, dependency) != DependencyMode::add_subdirectory)
                    continue;

                auto collected = collect_source_dependencies(graph, dependency, visited, packages);
                if (!collected)
                    return std::unexpected(collected.error());
            }

            packages.push_back(id);
            return {};
        }

        Result<void> collect_package_prefixes(
            const Graph& graph,
            const PackageId id,
            const BuildEnvironment& environment,
            const std::string_view variant,
            std::vector<bool>& visited,
            std::vector<bool>& added,
            std::vector<std::filesystem::path>& prefixes
        ) {
            if (visited[id.index])
                return {};
            visited[id.index] = true;

            const PackageNode& package = graph[id];
            auto options = read_options(graph, package);
            if (!options)
                return std::unexpected(options.error());

            for (const PackageId dependency: package.dependencies) {
                const PackageNode& target = graph[dependency];
                if (target.kind != PackageKind::managed || target.resolver != "cmake")
                    continue;

                if (dependency_mode(*options, dependency) == DependencyMode::find_package
                    && !added[dependency.index]) {
                    added[dependency.index] = true;
                    prefixes.push_back(artifact_directory(environment, target, variant));
                }

                auto collected = collect_package_prefixes(
                    graph,
                    dependency,
                    environment,
                    variant,
                    visited,
                    added,
                    prefixes
                );
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
            return
                "  set(_kaixa_product_directory " + cmake_quote(metadata) + ")\n"
                "  set(_kaixa_dependency_binary " + cmake_quote(dependencies) + ")\n"
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

        Result<std::vector<BuildProduct>> read_products(const PackageNode& package, const BuildContext& context) {
            const std::filesystem::path directory =
                product_metadata_directory(context) / context.configuration;
            std::error_code failure;
            const bool exists = std::filesystem::exists(directory, failure);
            if (failure) {
                return std::unexpected(error(
                    "cannot inspect CMake products in `" + directory.string()
                        + "`: " + failure.message()
                ));
            }
            if (!exists) {
                return std::unexpected(error(
                    "CMake target information is unavailable"
                ).add_note("run `kaixa generate` to configure the workspace"));
            }
            if (!std::filesystem::is_directory(directory, failure) || failure) {
                return std::unexpected(error(
                    "CMake product metadata is not a directory: " + directory.string()
                ));
            }

            std::vector<BuildProduct> products;
            std::filesystem::directory_iterator entries(directory, failure);
            if (failure) {
                return std::unexpected(error(
                    "cannot list CMake products in `" + directory.string()
                        + "`: " + failure.message()
                ));
            }

            for (const std::filesystem::directory_entry& entry: entries) {
                if (entry.path().extension() != ".product")
                    continue;

                std::ifstream input(entry.path(), std::ios::binary);
                std::string name;
                std::string type;
                std::string artifact;
                if (!input || !std::getline(input, name) || !std::getline(input, type)
                    || !std::getline(input, artifact)) {
                    return std::unexpected(error(
                        "cannot read CMake product metadata `" + entry.path().string() + "`"
                    ));
                }
                if (!name.empty() && name.back() == '\r')
                    name.pop_back();
                if (!type.empty() && type.back() == '\r')
                    type.pop_back();
                if (!artifact.empty() && artifact.back() == '\r')
                    artifact.pop_back();
                if (name.empty() || type.empty()) {
                    return std::unexpected(error(
                        "invalid CMake product metadata `" + entry.path().string() + "`"
                    ));
                }

                auto kind = product_kind(type);
                if (!kind)
                    return std::unexpected(std::move(kind).error().add_note(
                        "in `" + entry.path().string() + "`"
                    ));

                BuildProduct product{std::move(name), *kind, package.id, std::nullopt};
                if (!artifact.empty())
                    product.artifact = std::move(artifact);

                products.push_back(std::move(product));
            }

            std::ranges::sort(products, {}, &BuildProduct::name);
            return products;
        }

        std::string toml_string(const std::string_view value) {
            std::string result = "\"";
            for (const char character: value) {
                if (character == '\\' || character == '"')
                    result += '\\';
                if (character == '\n') {
                    result += "\\n";
                    continue;
                }

                result += character;
            }
            result += '"';
            return result;
        }

        void append_toml_array(
            std::string& output,
            const std::string_view name,
            const std::span<const std::string> values
        ) {
            output += std::string(name) + " = [";
            for (std::size_t index = 0; index < values.size(); ++index) {
                if (index != 0)
                    output += ", ";

                output += toml_string(values[index]);
            }
            output += "]\n";
        }

        std::string variant_metadata(const BuildEnvironment& environment, const BuildContext& context) {
            std::string output =
                "# Generated by Kaixa.\n"
                "resolver = \"cmake\"\n"
                "label = " + toml_string(context.variant.label) + "\n"
                "fingerprint = " + toml_string(context.variant.fingerprint) + "\n"
                "build = " + toml_string(context.directory.generic_string()) + "\n"
                "output = " + toml_string(context.output.generic_string()) + "\n"
                "profile = " + toml_string(environment.configuration.profile) + "\n"
                "source = " + toml_string(context.project.source.generic_string()) + "\n"
                "generation = " + toml_string(
                    context.project.generation == GenerationMode::source ? "source" : "state"
                ) + "\n";
            append_toml_array(output, "configs", environment.configuration.selected);
            output += "\n[cmake]\n";
            if (context.generator)
                output += "generator = " + toml_string(*context.generator) + "\n";
            if (context.build.c_compiler) {
                output += "c-compiler = "
                    + toml_string(*context.build.c_compiler) + "\n";
            }
            if (context.build.cxx_compiler) {
                output += "cxx-compiler = "
                    + toml_string(*context.build.cxx_compiler) + "\n";
            }
            if (context.build.toolchain) {
                output += "toolchain = "
                    + toml_string(context.build.toolchain->generic_string()) + "\n";
            }
            append_toml_array(output, "configure-arguments", context.build.configure_arguments);
            return output;
        }

        Result<BuildContext> prepare_build_context(
            const Graph& graph,
            const PackageNode& package,
            const BuildEnvironment& environment
        ) {
            auto project = read_options(graph, package);
            if (!project)
                return std::unexpected(project.error());

            const ResolverBuildConfiguration* configuration =
                environment.configuration.find("cmake");
            auto build = read_build_options(configuration && configuration->settings
                ? &*configuration->settings
                : nullptr
            );
            if (!build)
                return std::unexpected(build.error());

            if (configuration) {
                build->configure_arguments.insert(
                    build->configure_arguments.end(),
                    configuration->arguments.begin(),
                    configuration->arguments.end()
                );
                for (const ResolverArgumentGroup& scoped: configuration->scoped_arguments) {
                    std::vector<std::string>* destination = nullptr;
                    if (scoped.scope == "configure")
                        destination = &build->configure_arguments;
                    else if (scoped.scope == "build")
                        destination = &build->build_arguments;
                    else if (scoped.scope == "install")
                        destination = &build->install_arguments;
                    else {
                        return std::unexpected(error(
                            "unknown CMake argument scope `" + scoped.scope + "`"
                        ).add_note("expected configure, build or install"));
                    }

                    destination->insert(
                        destination->end(),
                        scoped.arguments.begin(),
                        scoped.arguments.end()
                    );
                }
            }

            std::optional<std::string> generator = requested_generator(build->configure_arguments);
            if (!generator)
                generator = build->generator;

            BuildVariant variant = build_variant(
                environment,
                *build,
                build->configure_arguments,
                *project
            );
            const std::filesystem::path directory =
                cmake_build_root(environment, variant.directory) / package.name;
            const std::filesystem::path output =
                environment.state_root / "build" / variant.directory;
            const std::filesystem::path metadata = package.id == graph.root()
                ? directory.parent_path() / "variant.toml"
                : directory.parent_path() / ".variants" / (package.name + ".toml");
            return BuildContext{
                std::move(*project),
                std::move(*build),
                std::move(generator),
                std::move(variant),
                configuration_name(environment.configuration.profile),
                directory,
                output,
                metadata
            };
        }

        Result<std::optional<std::string>> stored_fingerprint(const std::filesystem::path& metadata) {
            std::error_code failure;
            if (!std::filesystem::exists(metadata, failure)) {
                if (failure) {
                    return std::unexpected(error(
                        "cannot inspect CMake variant metadata `" + metadata.string()
                            + "`: " + failure.message()
                    ));
                }

                return std::nullopt;
            }

            std::ifstream input(metadata, std::ios::binary);
            if (!input) {
                return std::unexpected(error(
                    "cannot read CMake variant metadata `" + metadata.string() + "`"
                ));
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
                return std::unexpected(error(
                    "cannot inspect CMake build tree `" + context.directory.string()
                        + "`: " + failure.message()
                ));
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
            return "file(REMOVE_RECURSE " + cmake_quote(context.directory) + ")\n"
                + "file(MAKE_DIRECTORY " + cmake_quote(query.parent_path()) + ")\n"
                + "file(WRITE " + cmake_quote(query) + " \"\")\n";
        }

        Result<PreparedProject> prepare_project(
            const Graph& graph,
            const PackageNode& package,
            const BuildEnvironment& environment,
            const std::string_view variant,
            BuildPlan& plan
        ) {
            auto options = read_options(graph, package);
            if (!options)
                return std::unexpected(options.error());

            std::filesystem::path source = options->source;
            if (!options->targets.empty() && options->generation == GenerationMode::state) {
                source = environment.state_root / "generated" / "cmake"
                    / variant / package.name / "project";
            }
            const std::filesystem::path project = source / "CMakeLists.txt";
            if (!options->targets.empty()) {
                if (options->generation == GenerationMode::source
                    && std::filesystem::is_regular_file(project)) {
                    std::ifstream input(project, std::ios::binary);
                    std::string first_line;
                    if (!input || !std::getline(input, first_line)) {
                        return std::unexpected(error(
                            "cannot inspect existing `" + project.string() + "`"
                        ));
                    }
                    if (first_line.ends_with('\r'))
                        first_line.pop_back();
                    if (first_line != detail::generated_marker) {
                        SourceLocation location;
                        if (package.manifest)
                            location = package.manifest->location;
                        return std::unexpected(error_at(
                            std::move(location),
                            "refusing to overwrite `" + project.string()
                                + "` because it was not generated by Kaixa"
                        ));
                    }
                }
                plan.generate({project, detail::generate_project(package, *options)});
                return PreparedProject{std::move(source), project};
            }

            if (!std::filesystem::is_regular_file(project)) {
                SourceLocation location;
                if (package.manifest)
                    location = package.manifest->location;
                return std::unexpected(error_at(
                    std::move(location),
                    "CMake package `" + package.name + "` has no `" + project.string() + "`"
                ));
            }
            return PreparedProject{std::move(source), project};
        }

        class ResolverImpl final : public Resolver {
        public:
            [[nodiscard]] ResolverInfo info() const override {
                return {"cmake", "generates, adopts and composes CMake projects"};
            }

            [[nodiscard]] Result<void> plan(
                const Graph& graph,
                const PackageNode& package,
                const BuildEnvironment& environment,
                const BuildRequest& request,
                BuildPlan& plan
            ) const override {
                auto install = requires_install(graph, package);
                if (!install)
                    return std::unexpected(install.error());
                if (package.id != graph.root() && !*install)
                    return {};

                auto context = prepare_build_context(graph, package, environment);
                if (!context)
                    return std::unexpected(context.error());

                auto reset = requires_reset(*context);
                if (!reset)
                    return std::unexpected(reset.error());

                plan.generate({
                    context->metadata,
                    variant_metadata(environment, *context)
                });
                if (package.id == graph.root()) {
                    plan.output({package.id, "cmake", context->output});
                }

                std::vector<bool> source_visited(graph.size(), false);
                std::vector<PackageId> source_packages;
                auto source_result = collect_source_dependencies(
                    graph,
                    package.id,
                    source_visited,
                    source_packages
                );
                if (!source_result)
                    return std::unexpected(source_result.error());

                std::vector<std::optional<PreparedProject>> projects(graph.size());
                for (const PackageId id: source_packages) {
                    auto project = prepare_project(
                        graph,
                        graph[id],
                        environment,
                        context->variant.directory,
                        plan
                    );
                    if (!project)
                        return std::unexpected(project.error());
                    projects[id.index] = std::move(*project);
                }

                const std::filesystem::path integration_file =
                    environment.state_root / "generated" / "cmake"
                    / context->variant.directory / package.name / "dependencies.cmake";

                std::string integration =
                    "# Generated by Kaixa.\n"
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
                        + cmake_quote(projects[id.index]->source) + " "
                        + cmake_quote(context->directory / "_dependencies" / dependency.name)
                        + ")\n";
                }
                integration += "endif()\n";
                plan.generate({integration_file, std::move(integration)});
                plan.generate({detail::file_api_query(context->directory), {}});

                if (*reset) {
                    const std::filesystem::path script =
                        environment.state_root / "generated" / "cmake"
                        / context->variant.directory / package.name / "reset.cmake";
                    plan.generate({script, reset_script(*context)});

                    Action reset_action;
                    reset_action.description = "reset " + package.name;
                    reset_action.argv = {"cmake", "-P", script.string()};
                    reset_action.working_directory = package.directory;
                    reset_action.inputs.push_back(script);
                    reset_action.checked_state = ActionState::required;
                    reset_action.package = package.id;
                    reset_action.stage = ActionStage::synchronize;
                    plan.add(std::move(reset_action));
                }

                std::vector<bool> prefix_visited(graph.size(), false);
                std::vector<bool> prefix_added(graph.size(), false);
                std::vector<std::filesystem::path> prefixes;
                auto prefix_result = collect_package_prefixes(
                    graph,
                    package.id,
                    environment,
                    context->variant.directory,
                    prefix_visited,
                    prefix_added,
                    prefixes
                );
                if (!prefix_result)
                    return std::unexpected(prefix_result.error());

                Action configure;
                configure.description = "configure " + package.name;
                configure.argv = {
                    "cmake",
                    "-S", projects[package.id.index]->source.string(),
                    "-B", context->directory.string()
                };
                if (*install) {
                    configure.argv.push_back(
                        "-DCMAKE_INSTALL_PREFIX="
                            + artifact_directory(
                                environment,
                                package,
                                context->variant.directory
                            ).string()
                    );
                }
                configure.argv.push_back("-DCMAKE_PROJECT_INCLUDE=" + integration_file.string());
                configure.inputs.push_back(context->metadata);
                configure.inputs.push_back(integration_file);
                configure.argv.push_back("-DKAIXA_CMAKE_PREFIX_PATH=" + join_prefixes(prefixes));
                if (package.id == graph.root()) {
                    const std::string output = context->output.generic_string();
                    if (context->project.runtime_output || context->project.library_output
                        || context->project.archive_output) {
                        configure.argv.push_back("-DKAIXA_OUTPUT_ROOT=" + output);
                    }

                    configure.argv.push_back("-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=" + output + "/bin/$<0:>");
                    configure.argv.push_back("-DCMAKE_LIBRARY_OUTPUT_DIRECTORY=" + output + "/lib/$<0:>");
                    configure.argv.push_back("-DCMAKE_ARCHIVE_OUTPUT_DIRECTORY=" + output + "/lib/$<0:>");
                }
                if (!uses_multiple_configurations(context->generator))
                    configure.argv.push_back("-DCMAKE_BUILD_TYPE=" + context->configuration);
                if (context->build.generator
                    && !requested_generator(context->build.configure_arguments)) {
                    configure.argv.push_back("-G");
                    configure.argv.push_back(*context->build.generator);
                }
                if (context->build.c_compiler) {
                    configure.argv.push_back("-DCMAKE_C_COMPILER=" + *context->build.c_compiler);
                }
                if (context->build.cxx_compiler) {
                    configure.argv.push_back("-DCMAKE_CXX_COMPILER=" + *context->build.cxx_compiler);
                }
                if (context->build.toolchain) {
                    if (!std::filesystem::is_regular_file(*context->build.toolchain)) {
                        return std::unexpected(error(
                            "CMake toolchain file does not exist: "
                                + context->build.toolchain->string()
                        ));
                    }
                    configure.argv.push_back("-DCMAKE_TOOLCHAIN_FILE=" + context->build.toolchain->string());
                }
                configure.argv.insert(
                    configure.argv.end(),
                    context->build.configure_arguments.begin(),
                    context->build.configure_arguments.end()
                );
                configure.working_directory = package.directory;
                configure.package = package.id;
                configure.stage = ActionStage::synchronize;
                for (const PackageId id: source_packages)
                    configure.inputs.push_back(projects[id.index]->cmakelists);

                configure.outputs.push_back(context->directory / "CMakeCache.txt");
                auto checked_state = detail::configuration_state(
                    context->directory,
                    configure.inputs
                );
                configure.checked_state = *reset
                    ? ActionState::required
                    : (checked_state ? *checked_state : ActionState::unknown);
                plan.add(std::move(configure));

                Action build;
                build.description = "build " + package.name;
                build.argv = {
                    "cmake",
                    "--build", context->directory.string(),
                    "--config", context->configuration
                };
                build.working_directory = package.directory;
                build.inputs.push_back(context->directory / "CMakeCache.txt");
                build.outputs.push_back(context->directory);
                build.package = package.id;
                if (!request.targets.empty()) {
                    build.argv.push_back("--target");
                    build.argv.insert(build.argv.end(), request.targets.begin(), request.targets.end());
                }
                if (request.jobs) {
                    build.argv.push_back("--parallel");
                    build.argv.push_back(std::to_string(*request.jobs));
                }
                build.argv.insert(
                    build.argv.end(),
                    context->build.build_arguments.begin(),
                    context->build.build_arguments.end()
                );
                if (*install)
                    build.stage = ActionStage::synchronize;

                plan.add(std::move(build));

                if (*install) {
                    const std::filesystem::path destination = artifact_directory(
                        environment,
                        package,
                        context->variant.directory
                    );
                    Action install_action;
                    install_action.description = "install " + package.name;
                    install_action.argv = {
                        "cmake",
                        "--install", context->directory.string(),
                        "--config", context->configuration,
                        "--prefix", destination.string()
                    };
                    install_action.argv.insert(
                        install_action.argv.end(),
                        context->build.install_arguments.begin(),
                        context->build.install_arguments.end()
                    );
                    install_action.working_directory = package.directory;
                    install_action.inputs.push_back(context->directory);
                    install_action.outputs.push_back(destination);
                    install_action.package = package.id;
                    install_action.stage = ActionStage::synchronize;
                    plan.add(std::move(install_action));
                }
                return {};
            }

            [[nodiscard]] Result<void> plan_tests(
                const Graph& graph,
                const PackageNode& package,
                const BuildEnvironment& environment,
                const TestRequest& request,
                BuildPlan& plan
            ) const override {
                auto context = prepare_build_context(graph, package, environment);
                if (!context)
                    return std::unexpected(context.error());

                return detail::plan_tests(
                    context->project,
                    package,
                    context->directory,
                    context->configuration,
                    request,
                    plan
                );
            }

            [[nodiscard]] Result<std::vector<BuildProduct>> products(
                const Graph& graph,
                const PackageNode& package,
                const BuildEnvironment& environment
            ) const override {
                auto context = prepare_build_context(graph, package, environment);
                if (!context)
                    return std::unexpected(context.error());

                return read_products(package, *context);
            }

            [[nodiscard]] Result<void> plan_clean(
                const Graph& graph,
                const PackageNode& package,
                const BuildEnvironment& environment,
                const CleanRequest& request,
                CleanPlan& plan
            ) const override {
                auto context = prepare_build_context(graph, package, environment);
                if (!context)
                    return std::unexpected(context.error());

                plan.add(context->output);
                plan.add(cmake_build_root(environment, context->variant.directory));
                plan.add(
                    environment.state_root / "cache" / "cmake"
                        / context->variant.directory
                );
                if (request.generated_files) {
                    if (!context->project.targets.empty()
                        && context->project.generation == GenerationMode::source) {
                        plan.generated_file({
                            context->project.source / "CMakeLists.txt",
                            std::string(detail::generated_marker)
                        });
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
