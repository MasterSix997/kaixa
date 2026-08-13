#include "application.hpp"

#include <kaixa/foundation/process.hpp>
#include <kaixa/kaixa.hpp>
#include <kaixa/plugin/bundle.hpp>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
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
            if (root.manifest)
                layers.push_back(root.manifest->configurations);

            if (const auto user = user_configuration_path()) {
                auto loaded = append_configuration_file(layers, *user);
                if (!loaded)
                    return std::unexpected(loaded.error());
            }

            auto local = append_configuration_file(layers, directory / "Kaixa.user.toml");
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
                plugin::default_registry()
            };
        }

        void print_package(const Graph& graph, const PackageId id, const int depth) {
            const PackageNode& package = graph[id];
            std::cout << std::string(static_cast<std::size_t>(depth) * 2, ' ')
                << package.name;
            if (package.kind == PackageKind::opaque)
                std::cout << " (opaque)";
            else
                std::cout << " (" << package.resolver << ')';

            std::cout << '\n';
            for (const PackageId dependency: package.dependencies)
                print_package(graph, dependency, depth + 1);
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
                case ActionStage::synchronize: return "synchronization";
                case ActionStage::build: return "build";
                case ActionStage::test: return "test";
            }
            return "action";
        }

        Result<void> print_actions(
            const BuildPlan& plan,
            const bool synchronization_only = false
        ) {
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

        void print_outputs(const BuildPlan& plan) {
            for (const BuildOutput& output: plan.outputs())
                std::cout << "output: " << output.path.string() << '\n';
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
            auto graph = load_workspace(command.path);
            if (!graph)
                return fail(graph.error());

            print_package(*graph, graph->root(), 0);
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
                std::cout << state_name(file.state)
                    << " generated file: " << file.path.string() << '\n';
            }
            for (const ActionCheck& action: report->actions) {
                std::cout << state_name(action.state) << ' ' << stage_name(action.stage)
                    << ": " << action.description << '\n';
            }

            if (report->requires_synchronization()) {
                std::cout << "synchronization required\n";
                return 1;
            }

            std::cout << "workspace synchronized\n";
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

            for (const GeneratedFile& file: plan->generated_files())
                std::cout << "output file: " << file.path.string() << '\n';

            std::cout << "wrote " << report->written << " file(s); "
                << report->unchanged << " unchanged; synchronized "
                << report->synchronized << " action(s)\n";
            return 0;
        }

        int run(const BuildCommand& command) {
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

            auto printed = print_actions(*plan);
            if (!printed)
                return fail(printed.error());

            auto report = kaixa::execute(*plan);
            if (!report)
                return fail(report.error());

            std::cout << "completed " << report->executed << " action(s)\n";
            print_outputs(*plan);
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

            std::cout << "completed " << report->executed << " action(s)\n";
            print_outputs(*plan);
            return 0;
        }
    }

    int execute(const Command& command) {
        return std::visit([](const auto& value) {
            return run(value);
        }, command);
    }
}
