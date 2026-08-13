#include "application.hpp"
#include "configuration_output.hpp"

#include <kaixa/foundation/process.hpp>
#include <kaixa/kaixa.hpp>
#include <kaixa/plugin/bundle.hpp>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace kaixa::cli {
    namespace {
        struct Workspace {
            Graph graph;
            BuildEnvironment environment;
            ResolverRegistry registry;
            std::vector<ConfigurationSource> configuration_sources;
        };

        int fail(const Diagnostic& diagnostic) {
            std::cerr << format_diagnostic(diagnostic) << '\n';
            return 1;
        }

        std::optional<std::filesystem::path> user_configuration_path() {
#ifdef _WIN32
            const std::optional<std::string> base = environment_variable("APPDATA");
            if (base)
                return std::filesystem::path(*base) / "Kaixa" / "config.toml";
#else
            const std::optional<std::string> xdg = environment_variable("XDG_CONFIG_HOME");
            if (xdg)
                return std::filesystem::path(*xdg) / "kaixa" / "config.toml";

            const std::optional<std::string> home = environment_variable("HOME");
            if (home)
                return std::filesystem::path(*home) / ".config" / "kaixa" / "config.toml";
#endif
            return std::nullopt;
        }

        Result<void> append_configuration_file(
            std::vector<ConfigurationSet>& layers,
            std::vector<ConfigurationSource>& sources,
            std::string name,
            const std::filesystem::path& path
        ) {
            std::error_code failure;
            const bool exists = std::filesystem::exists(path, failure);
            if (failure == std::make_error_code(std::errc::no_such_file_or_directory))
                return {};
            if (failure) {
                return std::unexpected(error(
                    "cannot inspect configuration file `" + path.string() + "`: "
                        + failure.message()
                ));
            }
            if (!exists)
                return {};
            if (!std::filesystem::is_regular_file(path, failure) || failure) {
                return std::unexpected(error(
                    "configuration path is not a regular file: " + path.string()
                ));
            }

            auto configuration = parse_configuration_file(path);
            if (!configuration)
                return std::unexpected(configuration.error());

            sources.push_back({std::move(name), *configuration});
            layers.push_back(std::move(*configuration));
            return {};
        }

        bool resolver_is_active(const Graph& graph, const std::string_view resolver) {
            return std::ranges::any_of(graph.nodes(), [&](const PackageNode& package) {
                return package.kind == PackageKind::managed && package.resolver == resolver;
            });
        }

        Result<Workspace> open_workspace(const WorkspaceOptions& options) {
            auto graph = load_workspace(options.path);
            if (!graph)
                return std::unexpected(graph.error());

            for (const ResolverArgumentOverride& override: options.resolver_arguments) {
                if (!resolver_is_active(*graph, override.resolver)) {
                    return std::unexpected(error(
                        "resolver `" + override.resolver
                            + "` does not participate in this build"
                    ));
                }
            }

            const PackageNode& root = (*graph)[graph->root()];
            const std::filesystem::path directory = root.directory;
            std::vector<ConfigurationSet> layers;
            std::vector<ConfigurationSource> sources;
            if (root.manifest) {
                sources.push_back({"package", root.manifest->configurations});
                layers.push_back(root.manifest->configurations);
            }

            if (const auto user = user_configuration_path()) {
                auto loaded = append_configuration_file(layers, sources, "user", *user);
                if (!loaded)
                    return std::unexpected(loaded.error());
            }

            auto local = append_configuration_file(
                layers,
                sources,
                "local",
                directory / "Kaixa.user.toml"
            );
            if (!local)
                return std::unexpected(local.error());

            auto configuration = resolve_configurations(
                layers,
                options.configurations,
                options.profile,
                options.resolver_arguments
            );
            if (!configuration)
                return std::unexpected(configuration.error());

            return Workspace{
                std::move(*graph),
                BuildEnvironment{
                    directory,
                    directory / ".kaixa",
                    std::move(*configuration)
                },
                plugin::default_registry(),
                std::move(sources)
            };
        }

        Result<std::filesystem::path> find_workspace_directory(const std::filesystem::path& path) {
            std::error_code failure;
            std::filesystem::path directory = std::filesystem::absolute(path, failure);
            if (failure) {
                return std::unexpected(error(
                    "cannot resolve workspace path `" + path.string() + "`: "
                        + failure.message()
                ));
            }

            if (std::filesystem::is_regular_file(directory, failure))
                directory = directory.parent_path();
            else if (failure) {
                return std::unexpected(error(
                    "cannot inspect workspace path `" + directory.string() + "`: "
                        + failure.message()
                ));
            }

            while (!directory.empty()) {
                const std::filesystem::path manifest = directory / "Kaixa.toml";
                const bool found = std::filesystem::is_regular_file(manifest, failure);
                if (failure) {
                    return std::unexpected(error(
                        "cannot inspect workspace manifest `" + manifest.string() + "`: "
                            + failure.message()
                    ));
                }
                if (found)
                    return directory;

                const std::filesystem::path parent = directory.parent_path();
                if (parent == directory)
                    break;

                directory = parent;
            }
            return std::unexpected(error(
                "cannot find `Kaixa.toml` from `" + path.string() + "`"
            ));
        }

        Result<std::vector<std::filesystem::path>> existing_clean_paths(const CleanPlan& plan) {
            std::vector<std::filesystem::path> paths;
            for (const std::filesystem::path& path: plan.paths()) {
                std::error_code failure;
                const bool exists = std::filesystem::exists(path, failure);
                if (failure) {
                    return std::unexpected(error(
                        "cannot inspect clean path `" + path.string() + "`: "
                            + failure.message()
                    ));
                }
                if (exists)
                    paths.push_back(path);
            }
            for (const GeneratedCleanFile& generated: plan.generated_files()) {
                std::error_code failure;
                const bool exists = std::filesystem::exists(generated.path, failure);
                if (failure) {
                    return std::unexpected(error(
                        "cannot inspect generated file `" + generated.path.string() + "`: "
                            + failure.message()
                    ));
                }
                if (exists)
                    paths.push_back(generated.path);
            }
            return paths;
        }

        void print_package(
            const Graph& graph,
            const PackageId id,
            const int depth,
            const bool verbose = false
        ) {
            const PackageNode& package = graph[id];
            std::cout << std::string(static_cast<std::size_t>(depth) * 2, ' ')
                << package.name;
            if (package.kind == PackageKind::opaque)
                std::cout << " (opaque)";
            else
                std::cout << " (" << package.resolver << ')';
            if (verbose)
                std::cout << " -> " << package.directory.string();

            std::cout << '\n';
            for (const PackageId dependency: package.dependencies)
                print_package(graph, dependency, depth + 1, verbose);
        }

        std::string_view state_name(const GeneratedFileState state) {
            switch (state) {
                case GeneratedFileState::current: return "current";
                case GeneratedFileState::missing: return "missing";
                case GeneratedFileState::different: return "different";
            }
            return "unknown";
        }

        std::string_view state_name(const ActionState state) {
            switch (state) {
                case ActionState::current: return "current";
                case ActionState::required: return "required";
                case ActionState::unknown: return "unknown";
            }
            return "unknown";
        }

        std::string_view stage_name(const ActionStage stage) {
            switch (stage) {
                case ActionStage::synchronize: return "synchronize";
                case ActionStage::build: return "build";
                case ActionStage::test: return "test";
            }
            return "action";
        }

        std::string display_path(const std::filesystem::path& path, const std::filesystem::path& workspace) {
            const std::filesystem::path relative = path.lexically_relative(workspace);
            if (!relative.empty() && !relative.is_absolute()
                && *relative.begin() != "..") {
                return relative.generic_string();
            }

            return path.string();
        }

        bool path_exists(const std::filesystem::path& path) {
            std::error_code failure;
            return std::filesystem::is_regular_file(path, failure) && !failure;
        }

        void print_configuration_path(
            const std::string_view name,
            const std::filesystem::path& path,
            const std::filesystem::path& workspace
        ) {
            std::cout << name << ": " << display_path(path, workspace)
                << (path_exists(path) ? " [present]" : " [missing]") << '\n';
        }

        Result<void> print_actions(const BuildPlan& plan, const bool synchronization_only = false) {
            auto state = check(plan);
            if (!state)
                return std::unexpected(state.error());

            for (std::size_t index = 0; index < plan.actions().size(); ++index) {
                const Action& action = plan.actions()[index];
                if (synchronization_only && action.stage != ActionStage::synchronize)
                    continue;
                if (action.stage == ActionStage::synchronize
                    && state->actions[index].state == ActionState::current) {
                    continue;
                }

                std::cout << action.description << ": " << format_command(action.argv) << '\n';
            }

            std::cout.flush();
            return {};
        }

        void print_outputs(const BuildPlan& plan, const std::filesystem::path& workspace) {
            for (const BuildOutput& output: plan.outputs())
                std::cout << "artifact: " << display_path(output.path, workspace) << '\n';
        }

        void inspect_outputs(
            const Graph& graph,
            const BuildPlan& plan,
            const std::filesystem::path& workspace
        ) {
            if (plan.outputs().empty()) {
                std::cout << "no build outputs\n";
                return;
            }

            for (const BuildOutput& output: plan.outputs()) {
                std::cout << output.resolver << ' ' << graph[output.package].name << " -> "
                    << display_path(output.path, workspace) << '\n';
            }
        }

        Result<void> inspect_actions(
            const Graph& graph,
            const BuildPlan& plan,
            const std::filesystem::path& workspace,
            const bool verbose
        ) {
            auto report = check(plan);
            if (!report)
                return std::unexpected(report.error());

            if (plan.actions().empty()) {
                std::cout << "no build actions\n";
                return {};
            }

            for (std::size_t index = 0; index < plan.actions().size(); ++index) {
                const Action& action = plan.actions()[index];
                const ActionCheck& checked = report->actions[index];
                std::cout << state_name(checked.state) << ' ' << stage_name(action.stage) << ' ';
                if (action.package)
                    std::cout << graph[*action.package].name << ": ";

                std::cout << action.description << '\n';
                if (!verbose)
                    continue;

                std::cout << "  command: " << format_command(action.argv) << '\n';
                std::cout << "  working directory: "
                    << display_path(action.working_directory, workspace) << '\n';
                for (const std::filesystem::path& input: action.inputs)
                    std::cout << "  input: " << display_path(input, workspace) << '\n';
                for (const std::filesystem::path& output: action.outputs)
                    std::cout << "  output: " << display_path(output, workspace) << '\n';
            }
            return {};
        }

        std::string_view product_kind_name(const ProductKind kind) {
            switch (kind) {
                case ProductKind::executable: return "executable";
                case ProductKind::static_library: return "static-library";
                case ProductKind::shared_library: return "shared-library";
                case ProductKind::module_library: return "module-library";
                case ProductKind::object_library: return "object-library";
                case ProductKind::interface_library: return "interface-library";
                case ProductKind::utility: return "utility";
            }
            return "product";
        }

        void print_products(
            const std::span<const BuildProduct> products,
            const std::filesystem::path& workspace,
            const std::span<const std::string> selected = {}
        ) {
            for (const BuildProduct& product: products) {
                if (!selected.empty() && std::ranges::find(selected, product.name) == selected.end())
                    continue;

                std::cout << product_kind_name(product.kind) << ' ' << product.name;
                if (product.artifact)
                    std::cout << " -> " << display_path(*product.artifact, workspace);

                std::cout << '\n';
            }
        }

        Result<void> validate_build_targets(
            const std::span<const BuildProduct> products,
            const std::span<const std::string> requested
        ) {
            for (const std::string& name: requested) {
                if (std::ranges::none_of(products, [&](const BuildProduct& product) {
                        return product.name == name;
                    })) {
                    std::string available;
                    for (const BuildProduct& product: products) {
                        if (!available.empty())
                            available += ", ";

                        available += product.name;
                    }

                    Diagnostic diagnostic = error("build target `" + name + "` does not exist");
                    if (!available.empty())
                        return std::unexpected(std::move(diagnostic).add_note("available targets: " + available));

                    return std::unexpected(std::move(diagnostic));
                }
            }
            return {};
        }

        int run(const HelpCommand&) {
            print_usage(std::cout);
            return 0;
        }

        int run(const VersionCommand&) {
            std::cout << "kaixa " << version() << '\n';
            return 0;
        }

        int run(const InspectCommand& command) {
            if (command.mode == InspectMode::packages) {
                auto graph = load_workspace(command.workspace.path);
                if (!graph)
                    return fail(graph.error());

                print_package(*graph, graph->root(), 0, command.verbose);
                return 0;
            }

            auto workspace = open_workspace(command.workspace);
            if (!workspace)
                return fail(workspace.error());

            if (command.mode == InspectMode::config) {
                const PackageNode& root = workspace->graph[workspace->graph.root()];
                print_effective_configuration(
                    workspace->environment.configuration,
                    workspace->configuration_sources,
                    root.resolver,
                    workspace->environment.workspace,
                    command.verbose
                );
                return 0;
            }

            auto plan = plan_build(workspace->graph, workspace->registry, workspace->environment);
            if (!plan)
                return fail(plan.error());

            if (command.mode == InspectMode::outputs) {
                inspect_outputs(workspace->graph, *plan, workspace->environment.workspace);
                return 0;
            }
            if (command.mode == InspectMode::actions) {
                auto printed = inspect_actions(
                    workspace->graph,
                    *plan,
                    workspace->environment.workspace,
                    command.verbose
                );
                return printed ? 0 : fail(printed.error());
            }

            auto state = check(*plan);
            if (!state)
                return fail(state.error());
            if (state->requires_synchronization()) {
                return fail(error("target information is not synchronized")
                    .add_note("run `kaixa generate` before inspecting targets"));
            }

            auto products = discover_products(
                workspace->graph,
                workspace->registry,
                workspace->environment
            );
            if (!products)
                return fail(products.error());

            print_products(*products, workspace->environment.workspace);
            return 0;
        }

        int run(const CheckCommand& command) {
            auto workspace = open_workspace(command.workspace);
            if (!workspace)
                return fail(workspace.error());

            auto plan = plan_build(
                workspace->graph,
                workspace->registry,
                workspace->environment
            );
            if (!plan)
                return fail(plan.error());

            auto report = check(*plan);
            if (!report)
                return fail(report.error());

            for (const GeneratedFileCheck& file: report->generated_files) {
                if (file.state == GeneratedFileState::current)
                    continue;

                std::cout << "generated file: " << state_name(file.state) << ' '
                    << display_path(file.path, workspace->environment.workspace) << '\n';
            }
            for (const ActionCheck& action: report->actions) {
                if (action.stage == ActionStage::synchronize && action.state == ActionState::required)
                    std::cout << "required synchronization: " << action.description << '\n';
            }

            if (report->requires_synchronization()) {
                std::cout << "workspace requires synchronization; run `kaixa generate`\n";
                return 1;
            }

            std::cout << "workspace is synchronized\n";
            return 0;
        }

        int run(const GenerateCommand& command) {
            auto workspace = open_workspace(command.workspace);
            if (!workspace)
                return fail(workspace.error());

            auto plan = plan_build(
                workspace->graph,
                workspace->registry,
                workspace->environment
            );
            if (!plan)
                return fail(plan.error());

            auto printed = print_actions(*plan, true);
            if (!printed)
                return fail(printed.error());

            auto report = generate(*plan);
            if (!report)
                return fail(report.error());

            std::cout << "workspace synchronized: " << report->written
                << " file(s) written, " << report->unchanged << " unchanged, "
                << report->synchronized << " action(s) run\n";
            return 0;
        }

        int run(const BuildCommand& command) {
            auto workspace = open_workspace(command.workspace);
            if (!workspace)
                return fail(workspace.error());

            if (command.list || !command.targets.empty()) {
                auto synchronization = plan_build(
                    workspace->graph,
                    workspace->registry,
                    workspace->environment
                );
                if (!synchronization)
                    return fail(synchronization.error());

                auto printed = print_actions(*synchronization, true);
                if (!printed)
                    return fail(printed.error());

                auto generated = generate(*synchronization);
                if (!generated)
                    return fail(generated.error());

                auto products = discover_products(
                    workspace->graph,
                    workspace->registry,
                    workspace->environment
                );
                if (!products)
                    return fail(products.error());

                if (command.list) {
                    print_products(*products, workspace->environment.workspace);
                    return 0;
                }

                auto valid = validate_build_targets(*products, command.targets);
                if (!valid)
                    return fail(valid.error());
            }

            BuildRequest request;
            request.targets = command.targets;
            request.jobs = command.jobs;
            auto plan = plan_build(
                workspace->graph,
                workspace->registry,
                workspace->environment,
                request
            );
            if (!plan)
                return fail(plan.error());

            auto printed = print_actions(*plan);
            if (!printed)
                return fail(printed.error());

            auto report = kaixa::execute(*plan);
            if (!report)
                return fail(report.error());

            std::cout << "build completed: " << report->executed << " action(s) run\n";
            auto products = discover_products(
                workspace->graph,
                workspace->registry,
                workspace->environment
            );
            if (!products)
                return fail(products.error());

            print_products(*products, workspace->environment.workspace, command.targets);
            return 0;
        }

        int run(const TestCommand& command) {
            auto workspace = open_workspace(command.workspace);
            if (!workspace)
                return fail(workspace.error());

            auto plan = plan_tests(
                workspace->graph,
                workspace->registry,
                workspace->environment,
                command.request
            );
            if (!plan)
                return fail(plan.error());

            auto printed = print_actions(*plan);
            if (!printed)
                return fail(printed.error());

            auto report = test(*plan);
            if (!report)
                return fail(report.error());

            std::cout << "tests completed: " << report->executed << " action(s) run\n";
            print_outputs(*plan, workspace->environment.workspace);
            return 0;
        }

        int run(const RunCommand& command) {
            auto workspace = open_workspace(command.workspace);
            if (!workspace)
                return fail(workspace.error());

            auto synchronization = plan_build(
                workspace->graph,
                workspace->registry,
                workspace->environment
            );
            if (!synchronization)
                return fail(synchronization.error());

            auto printed = print_actions(*synchronization, true);
            if (!printed)
                return fail(printed.error());

            auto generated = generate(*synchronization);
            if (!generated)
                return fail(generated.error());

            auto targets = discover_run_targets(
                workspace->graph,
                workspace->registry,
                workspace->environment
            );
            if (!targets)
                return fail(targets.error());

            if (command.list) {
                if (targets->empty()) {
                    std::cout << "no runnable targets\n";
                    return 0;
                }

                for (const RunTarget& target: *targets)
                    std::cout << target.name << '\n';

                return 0;
            }

            const PackageNode& root = workspace->graph[workspace->graph.root()];
            auto selected = select_run_target(*targets, command.target, root.name);
            if (!selected)
                return fail(selected.error());

            auto plan = plan_run(
                workspace->graph,
                workspace->registry,
                workspace->environment,
                selected->name
            );
            if (!plan)
                return fail(plan.error());

            printed = print_actions(*plan);
            if (!printed)
                return fail(printed.error());

            auto built = kaixa::execute(*plan);
            if (!built)
                return fail(built.error());

            selected->process.argv.insert(
                selected->process.argv.end(),
                command.arguments.begin(),
                command.arguments.end()
            );
            std::cout << "running: " << format_command(selected->process.argv) << '\n';
            std::cout.flush();

            auto result = run_process(selected->process);
            if (!result)
                return fail(result.error());

            return result->exit_code;
        }

        int run(const CleanCommand& command) {
            CleanPlan plan;
            std::filesystem::path state_root;
            std::filesystem::path workspace_directory;
            if (command.all) {
                auto directory = find_workspace_directory(command.workspace.path);
                if (!directory)
                    return fail(directory.error());

                workspace_directory = *directory;
                state_root = workspace_directory / ".kaixa";
                plan.add(state_root);
                if (command.generated_files) {
                    auto workspace = open_workspace(command.workspace);
                    if (!workspace)
                        return fail(workspace.error());

                    auto generated = plan_clean(
                        workspace->graph,
                        workspace->registry,
                        workspace->environment,
                        CleanRequest{true}
                    );
                    if (!generated)
                        return fail(generated.error());

                    for (const GeneratedCleanFile& file: generated->generated_files())
                        plan.generated_file(file);
                }
            } else {
                auto workspace = open_workspace(command.workspace);
                if (!workspace)
                    return fail(workspace.error());

                workspace_directory = workspace->environment.workspace;
                state_root = workspace->environment.state_root;
                auto planned = plan_clean(
                    workspace->graph,
                    workspace->registry,
                    workspace->environment,
                    CleanRequest{command.generated_files}
                );
                if (!planned)
                    return fail(planned.error());

                plan = std::move(*planned);
            }

            auto existing = existing_clean_paths(plan);
            if (!existing)
                return fail(existing.error());

            auto report = clean(
                plan,
                state_root,
                workspace_directory,
                command.dry_run,
                command.all
            );
            if (!report)
                return fail(report.error());

            if (existing->empty()) {
                std::cout << "nothing to clean\n";
                return 0;
            }

            for (const std::filesystem::path& path: *existing) {
                std::cout << (command.dry_run ? "would remove: " : "removed: ")
                    << display_path(path, workspace_directory) << '\n';
            }
            if (!command.dry_run) {
                std::cout << "removed " << report->removed_entries
                    << " filesystem entry(s)\n";
            }
            return 0;
        }

        int run(const ConfigListCommand& command) {
            WorkspaceOptions options;
            options.path = command.path;
            auto workspace = open_workspace(options);
            if (!workspace)
                return fail(workspace.error());

            print_configuration_list(
                workspace->configuration_sources,
                workspace->environment.configuration,
                workspace->environment.workspace
            );
            return 0;
        }

        int run(const ConfigShowCommand& command) {
            auto workspace = open_workspace(command.workspace);
            if (!workspace)
                return fail(workspace.error());

            const PackageNode& root = workspace->graph[workspace->graph.root()];
            print_effective_configuration(
                workspace->environment.configuration,
                workspace->configuration_sources,
                root.resolver,
                workspace->environment.workspace,
                command.verbose
            );
            return 0;
        }

        int run(const ConfigPathCommand& command) {
            auto directory = find_workspace_directory(command.path);
            if (!directory)
                return fail(directory.error());

            print_configuration_path("package", *directory / "Kaixa.toml", *directory);
            if (const auto user = user_configuration_path())
                print_configuration_path("user", *user, *directory);
            else
                std::cout << "user: unavailable\n";

            print_configuration_path("local", *directory / "Kaixa.user.toml", *directory);
            return 0;
        }
    }

    int execute(const Command& command) {
        return std::visit([](const auto& value) {
            return run(value);
        }, command);
    }
}
