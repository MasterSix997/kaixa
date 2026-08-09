#include <kaixa/plugin/cmake/resolver.hpp>

#include <kaixa/config/table_reader.hpp>
#include <kaixa/foundation/process.hpp>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kaixa::plugin::cmake {
    namespace {
        enum class DependencyMode {
            add_subdirectory,
            find_package
        };

        struct DependencyOption {
            PackageId package;
            DependencyMode mode = DependencyMode::add_subdirectory;
        };

        struct Options {
            std::filesystem::path source;
            std::optional<std::string> generator;
            std::vector<DependencyOption> dependencies;
        };

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

        Result<Options> read_options(const Graph& graph, const PackageNode& package) {
            Options result{package.directory, std::nullopt, {}};
            if (!package.manifest || !package.manifest->resolver_options)
                return result;

            auto options_result = TableReader::bind(*package.manifest->resolver_options, "cmake");
            if (!options_result)
                return std::unexpected(options_result.error());
            TableReader options = std::move(*options_result);

            auto source = options.optional_string("source");
            if (!source)
                return std::unexpected(source.error());
            if (*source)
                result.source /= **source;

            auto generator = options.optional_string("generator");
            if (!generator)
                return std::unexpected(generator.error());
            result.generator = std::move(*generator);

            auto dependencies_result = options.optional_table("dependencies");
            if (!dependencies_result)
                return std::unexpected(dependencies_result.error());
            if (*dependencies_result) {
                TableReader dependencies = std::move(**dependencies_result);
                for (const TableEntry& entry: dependencies.entries()) {
                    const std::string* mode_name = entry.value.as_string();
                    SourceLocation location = entry.value.location();
                    location.config_path = join_config_path(dependencies.path(), entry.key);
                    if (!mode_name) {
                        return std::unexpected(error_at(
                            std::move(location),
                            "expected a string, found "
                                + std::string(value_kind_name(entry.value.kind()))
                        ));
                    }

                    const auto dependency = std::ranges::find_if(
                        package.dependencies,
                        [&](const PackageId id) { return graph[id].name == entry.key; }
                    );
                    if (dependency == package.dependencies.end()) {
                        return std::unexpected(error_at(
                            std::move(location),
                            "`" + entry.key + "` is not a dependency of `" + package.name + "`"
                        ));
                    }
                    if (graph[*dependency].kind != PackageKind::managed
                        || graph[*dependency].resolver != "cmake") {
                        return std::unexpected(error_at(
                            std::move(location),
                            "CMake integration can only be selected for a managed CMake dependency"
                        ));
                    }

                    DependencyMode mode;
                    if (*mode_name == "add-subdirectory") {
                        mode = DependencyMode::add_subdirectory;
                    } else if (*mode_name == "find-package") {
                        mode = DependencyMode::find_package;
                    } else {
                        return std::unexpected(error_at(
                            std::move(location),
                            "unknown CMake dependency mode `" + *mode_name
                                + "`; expected `add-subdirectory` or `find-package`"
                        ));
                    }
                    result.dependencies.push_back({*dependency, mode});
                }
                dependencies.take_all();
            }

            auto finished = options.finish();
            if (!finished)
                return std::unexpected(finished.error());
            return result;
        }

        DependencyMode dependency_mode(const Options& options, const PackageId dependency) {
            const auto selected = std::ranges::find_if(
                options.dependencies,
                [&](const DependencyOption& option) { return option.package == dependency; }
            );
            return selected == options.dependencies.end()
                ? DependencyMode::add_subdirectory
                : selected->mode;
        }

        std::filesystem::path cmake_build_root(const BuildEnvironment& environment) {
            return environment.state_root / "build" / "cmake";
        }

        std::filesystem::path artifact_directory(
            const BuildEnvironment& environment,
            const PackageNode& package
        ) {
            return environment.state_root / "artifacts" / "cmake" / package.name;
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
                    prefixes.push_back(artifact_directory(environment, target));
                }

                auto collected = collect_package_prefixes(
                    graph,
                    dependency,
                    environment,
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

        Result<std::filesystem::path> validate_project(
            const Graph& graph,
            const PackageNode& package
        ) {
            auto options = read_options(graph, package);
            if (!options)
                return std::unexpected(options.error());

            const std::filesystem::path project = options->source / "CMakeLists.txt";
            if (!std::filesystem::is_regular_file(project)) {
                SourceLocation location;
                if (package.manifest)
                    location = package.manifest->location;
                return std::unexpected(error_at(
                    std::move(location),
                    "CMake package `" + package.name + "` has no `" + project.string() + "`"
                ));
            }
            return project;
        }

        class ResolverImpl final : public Resolver {
        public:
            [[nodiscard]] ResolverInfo info() const override {
                return {"cmake", "adopts and composes existing CMake projects"};
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

                auto root_options = read_options(graph, package);
                if (!root_options)
                    return std::unexpected(root_options.error());

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

                for (const PackageId id: source_packages) {
                    auto project = validate_project(graph, graph[id]);
                    if (!project)
                        return std::unexpected(project.error());
                }

                const std::filesystem::path build_directory =
                    cmake_build_root(environment) / package.name;
                const std::filesystem::path integration_file =
                    environment.state_root / "generated" / "cmake"
                    / package.name / "dependencies.cmake";

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
                    auto options = read_options(graph, dependency);
                    if (!options)
                        return std::unexpected(options.error());

                    integration += "  add_subdirectory(" + cmake_quote(options->source) + " "
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
                    prefix_visited,
                    prefix_added,
                    prefixes
                );
                if (!prefix_result)
                    return std::unexpected(prefix_result.error());

                const std::string configuration = configuration_name(environment.profile);
                Action configure;
                configure.description = "configure " + package.name;
                configure.argv = {
                    "cmake",
                    "-S", root_options->source.string(),
                    "-B", build_directory.string()
                };
                if (*install) {
                    configure.argv.push_back(
                        "-DCMAKE_INSTALL_PREFIX=" + artifact_directory(environment, package).string()
                    );
                }
                configure.argv.push_back(
                    "-DCMAKE_PROJECT_INCLUDE=" + integration_file.string()
                );
                configure.inputs.push_back(integration_file);
                configure.argv.push_back("-DKAIXA_CMAKE_PREFIX_PATH=" + join_prefixes(prefixes));
                if (!uses_multiple_configurations(root_options->generator))
                    configure.argv.push_back("-DCMAKE_BUILD_TYPE=" + configuration);
                if (root_options->generator) {
                    configure.argv.push_back("-G");
                    configure.argv.push_back(*root_options->generator);
                }
                configure.working_directory = package.directory;
                configure.inputs.push_back(root_options->source / "CMakeLists.txt");
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
                    const std::filesystem::path destination = artifact_directory(environment, package);
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
