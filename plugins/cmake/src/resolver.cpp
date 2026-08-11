#include <kaixa/plugin/cmake/resolver.hpp>

#include <kaixa/foundation/process.hpp>

#include "configuration.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
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

        std::optional<std::string> requested_generator(
            const std::vector<std::string>& arguments
        ) {
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

        std::string build_variant(
            const BuildEnvironment& environment,
            const detail::BuildOptions& options,
            const std::vector<std::string>& arguments
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
            std::string profile = environment.configuration.profile;
            for (char& character: profile) {
                const bool valid = (character >= 'a' && character <= 'z')
                    || (character >= 'A' && character <= 'Z')
                    || (character >= '0' && character <= '9')
                    || character == '-' || character == '_';
                if (!valid)
                    character = '_';
            }
            return profile + '-' + std::string(encoded, converted.ptr);
        }

        std::filesystem::path cmake_build_root(
            const BuildEnvironment& environment,
            const std::string_view variant
        ) {
            return environment.state_root / "build" / "cmake" / variant;
        }

        std::filesystem::path artifact_directory(
            const BuildEnvironment& environment,
            const PackageNode& package,
            const std::string_view variant
        ) {
            return environment.state_root / "artifacts" / "cmake" / variant / package.name;
        }

        Result<bool> requires_install(
            const Graph& graph,
            const PackageNode& package
        ) {
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
                BuildPlan& plan
            ) const override {
                auto install = requires_install(graph, package);
                if (!install)
                    return std::unexpected(install.error());
                if (package.id != graph.root() && !*install)
                    return {};

                const ResolverBuildConfiguration* resolver_configuration =
                    environment.configuration.find("cmake");
                auto build_options = read_build_options(
                    resolver_configuration && resolver_configuration->settings
                        ? &*resolver_configuration->settings
                        : nullptr
                );
                if (!build_options)
                    return std::unexpected(build_options.error());

                std::vector<std::string> resolver_arguments = build_options->arguments;
                if (resolver_configuration) {
                    resolver_arguments.insert(
                        resolver_arguments.end(),
                        resolver_configuration->arguments.begin(),
                        resolver_configuration->arguments.end()
                    );
                }
                std::optional<std::string> generator = requested_generator(resolver_arguments);
                if (!generator)
                    generator = build_options->generator;
                const std::string variant = build_variant(
                    environment,
                    *build_options,
                    resolver_arguments
                );

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
                        variant,
                        plan
                    );
                    if (!project)
                        return std::unexpected(project.error());
                    projects[id.index] = std::move(*project);
                }

                const std::filesystem::path build_directory =
                    cmake_build_root(environment, variant) / package.name;
                const std::filesystem::path integration_file =
                    environment.state_root / "generated" / "cmake"
                    / variant / package.name / "dependencies.cmake";

                std::string integration =
                    "# Generated by Kaixa.\n"
                    "if(KAIXA_CMAKE_PREFIX_PATH)\n"
                    "  list(PREPEND CMAKE_PREFIX_PATH ${KAIXA_CMAKE_PREFIX_PATH})\n"
                    "endif()\n"
                    "if(NOT KAIXA_CMAKE_DEPENDENCIES_INCLUDED)\n"
                    "  set(KAIXA_CMAKE_DEPENDENCIES_INCLUDED TRUE)\n";
                for (const PackageId id: source_packages) {
                    if (id == package.id)
                        continue;
                    const PackageNode& dependency = graph[id];

                    integration += "  add_subdirectory("
                        + cmake_quote(projects[id.index]->source) + " "
                        + cmake_quote(build_directory / "_dependencies" / dependency.name)
                        + ")\n";
                }
                integration += "endif()\n";
                plan.generate({integration_file, std::move(integration)});

                std::vector<bool> prefix_visited(graph.size(), false);
                std::vector<bool> prefix_added(graph.size(), false);
                std::vector<std::filesystem::path> prefixes;
                auto prefix_result = collect_package_prefixes(
                    graph,
                    package.id,
                    environment,
                    variant,
                    prefix_visited,
                    prefix_added,
                    prefixes
                );
                if (!prefix_result)
                    return std::unexpected(prefix_result.error());

                const std::string configuration = configuration_name(
                    environment.configuration.profile
                );
                Action configure;
                configure.description = "configure " + package.name;
                configure.argv = {
                    "cmake",
                    "-S", projects[package.id.index]->source.string(),
                    "-B", build_directory.string()
                };
                if (*install) {
                    configure.argv.push_back(
                        "-DCMAKE_INSTALL_PREFIX="
                            + artifact_directory(environment, package, variant).string()
                    );
                }
                configure.argv.push_back(
                    "-DCMAKE_PROJECT_INCLUDE=" + integration_file.string()
                );
                configure.inputs.push_back(integration_file);
                configure.argv.push_back("-DKAIXA_CMAKE_PREFIX_PATH=" + join_prefixes(prefixes));
                if (!uses_multiple_configurations(generator))
                    configure.argv.push_back("-DCMAKE_BUILD_TYPE=" + configuration);
                if (build_options->generator && !requested_generator(resolver_arguments)) {
                    configure.argv.push_back("-G");
                    configure.argv.push_back(*build_options->generator);
                }
                if (build_options->c_compiler)
                    configure.argv.push_back("-DCMAKE_C_COMPILER=" + *build_options->c_compiler);
                if (build_options->cxx_compiler) {
                    configure.argv.push_back(
                        "-DCMAKE_CXX_COMPILER=" + *build_options->cxx_compiler
                    );
                }
                if (build_options->toolchain) {
                    if (!std::filesystem::is_regular_file(*build_options->toolchain)) {
                        return std::unexpected(error(
                            "CMake toolchain file does not exist: "
                                + build_options->toolchain->string()
                        ));
                    }
                    configure.argv.push_back(
                        "-DCMAKE_TOOLCHAIN_FILE=" + build_options->toolchain->string()
                    );
                }
                configure.argv.insert(
                    configure.argv.end(),
                    resolver_arguments.begin(),
                    resolver_arguments.end()
                );
                configure.working_directory = package.directory;
                configure.inputs.push_back(projects[package.id.index]->cmakelists);
                configure.outputs.push_back(build_directory / "CMakeCache.txt");
                plan.add(std::move(configure));

                Action build;
                build.description = "build " + package.name;
                build.argv = {
                    "cmake",
                    "--build", build_directory.string(),
                    "--config", configuration
                };
                build.working_directory = package.directory;
                build.inputs.push_back(build_directory / "CMakeCache.txt");
                build.outputs.push_back(build_directory);
                plan.add(std::move(build));

                if (*install) {
                    const std::filesystem::path destination = artifact_directory(
                        environment,
                        package,
                        variant
                    );
                    Action install_action;
                    install_action.description = "install " + package.name;
                    install_action.argv = {
                        "cmake",
                        "--install", build_directory.string(),
                        "--config", configuration,
                        "--prefix", destination.string()
                    };
                    install_action.working_directory = package.directory;
                    install_action.inputs.push_back(build_directory);
                    install_action.outputs.push_back(destination);
                    plan.add(std::move(install_action));
                }
                return {};
            }
        };
    }

    std::unique_ptr<Resolver> make_resolver() {
        return std::make_unique<ResolverImpl>();
    }
}
