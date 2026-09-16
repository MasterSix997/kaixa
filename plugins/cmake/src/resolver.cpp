#include <kaixa/plugin/cmake/resolver.hpp>

#include <kaixa/config/value_operations.hpp>
#include <kaixa/extension/registry.hpp>
#include <kaixa/foundation/process.hpp>

#include <configuration.hpp>
#include <discovery/file_api.hpp>
#include <discovery/products.hpp>
#include <generation/cmake_syntax.hpp>
#include <generation/integration.hpp>
#include <generation/portable_project.hpp>
#include <generation/project.hpp>
#include <model/policies.hpp>
#include <planning/actions.hpp>
#include <planning/build_context.hpp>
#include <planning/variants.hpp>
#include <schema/project_options.hpp>
#include <testing/testing.hpp>

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
        using detail::append_build_action;
        using detail::artifact_directory;
        using detail::build_action;
        using detail::build_variant;
        using detail::BuildContext;
        using detail::BuildVariant;
        using detail::cmake_build_root;
        using detail::configure_action;
        using detail::ConfigureActionContext;
        using detail::ConfiguredRoute;
        using detail::dependency_integration;
        using detail::DependencyIntegrationContext;
        using detail::install_action;
        using detail::PreparedProject;
        using detail::product_metadata_directory;
        using detail::read_products;
        using detail::requested_generator;
        using detail::RoutePlanningContext;
        using detail::uses_multiple_configurations;

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
                if (package.manifest()) {
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
                    if (!candidate.has_build_semantics() || candidate.resolver != "cmake")
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
                if (!target.has_build_semantics() || target.resolver != "cmake")
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
                        if (!target.has_build_semantics() || target.resolver != "cmake")
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
                if (!target.has_build_semantics() || target.resolver != "cmake")
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
            ExecutionPlan& plan;
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
                        if (package.manifest())
                            location = package.manifest()->location;

                        return std::unexpected(error_at(
                            std::move(location),
                            "refusing to overwrite `" + project.string() + "` because it was not generated by Kaixa"
                        ));
                    }
                }
                std::vector<detail::ProjectPackage> packages;
                if (options.generation == GenerationMode::export_project) {
                    for (const PackageNode& candidate: context.graph.nodes()) {
                        if (!candidate.has_build_semantics()
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
                if (package.manifest())
                    location = package.manifest()->location;

                return std::unexpected(
                    error_at(std::move(location), "CMake package `" + package.name + "` has no `" + project.string() + "`")
                );
            }
            return PreparedProject{std::move(source), project};
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
            action.output = ProcessOutputMode::stream;
            action.description = "reset " + planning.package.name;
            action.argv = {"cmake", "-P", script.string()};
            action.working_directory = planning.package.directory;
            action.inputs.push_back(script);
            action.checked_state = ActionState::required;
            action.package = planning.package.id;
            action.configured_artifact = route.instance->artifact;
            planning.plan.synchronize(std::move(action));
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

            planning.plan.synchronize(std::move(*configure));
            if (route.request.build_default) {
                append_build_action(
                    planning.plan,
                    build_action(planning.package, build, *route.instance, route.request, false),
                    install.has_value()
                );
            }
            if (!route.request.targets.empty()) {
                append_build_action(
                    planning.plan,
                    build_action(planning.package, build, *route.instance, route.request, true),
                    install.has_value()
                );
            }
            if (!route.request.build_default && route.request.targets.empty()) {
                return std::unexpected(error("CMake build request selects neither default nor explicit targets"));
            }
            if (install)
                planning.plan.synchronize(install_action(planning.package, build, *route.instance, *install));

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

            [[nodiscard]] std::span<const PolicyDefinition> policies() const override { return native_policy_definitions(); }

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
                ExecutionPlan& plan,
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
            [[nodiscard]] Result<void> plan_route(
                const ConfiguredRoute& route,
                RoutePlanningContext& planning,
                const bool force_build = false
            ) const {
                const ProductRealizationContext realization{planning.environment.configuration.profile, host_target_os()};
                auto
                    dependency_install = requires_install(planning.graph, planning.registry, planning.package, realization, planning.cache);
                if (!dependency_install)
                    return std::unexpected(dependency_install.error());

                const std::optional<std::filesystem::path>
                    install = install_destination(route.request, *dependency_install, planning.environment, *route.instance);
                if (!force_build && !planning.graph.is_root(planning.package.id) && !install)
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
                ExecutionPlan& plan,
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
                    const bool has_build_action = std::ranges::any_of(plan.builds(), [&](const Action& action) {
                        return action.package == package.id && action.configured_artifact == route.instance->artifact;
                    });
                    if (!has_build_action) {
                        ConfiguredRoute build_route = route;
                        build_route.request.targets.clear();
                        build_route.request.build_default = true;
                        auto planned = plan_route(build_route, planning, true);
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
