#include "application.hpp"
#include "application_internal.hpp"
#include "configuration_output.hpp"

#include <kaixa/foundation/filesystem.hpp>
#include <kaixa/foundation/process.hpp>
#include <kaixa/kaixa.hpp>
#include <kaixa/model/effective_product.hpp>
#include <kaixa/package/manager.hpp>
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
    namespace detail {
        int fail(const Diagnostic& diagnostic) {
            std::cerr << format_diagnostic(diagnostic) << '\n';
            return 1;
        }

        int fail(const Diagnostic& diagnostic, const DiagnosticFormat format) {
            if (format == DiagnosticFormat::short_form) {
                std::cerr << format_diagnostic_short(diagnostic) << '\n';
                return 1;
            }
            return fail(diagnostic);
        }

        Result<PackageId> require_single_root(const Graph& graph, const std::string_view operation) {
            if (graph.roots().size() == 1)
                return graph.roots().front();

            return std::unexpected(error(std::string(operation) + " requires exactly one selected package")
                    .add_note("select one package with `--package <name>`"));
        }

        Result<std::filesystem::path> find_workspace_directory(const std::filesystem::path& path) {
            std::error_code failure;
            std::filesystem::path directory = std::filesystem::absolute(path, failure);
            if (failure) {
                return std::unexpected(error("cannot resolve workspace path `" + path.string() + "`: " + failure.message()));
            }

            if (std::filesystem::is_regular_file(directory, failure))
                directory = directory.parent_path();
            else if (failure) {
                return std::unexpected(error("cannot inspect workspace path `" + directory.string() + "`: " + failure.message()));
            }

            while (!directory.empty()) {
                const std::filesystem::path manifest = directory / "Kaixa.toml";
                const bool found = std::filesystem::is_regular_file(manifest, failure);
                if (failure) {
                    return std::unexpected(error("cannot inspect workspace manifest `" + manifest.string() + "`: " + failure.message()));
                }
                if (found)
                    return directory;

                const std::filesystem::path parent = directory.parent_path();
                if (parent == directory)
                    break;

                directory = parent;
            }
            return std::unexpected(error("cannot find `Kaixa.toml` from `" + path.string() + "`"));
        }

        Result<std::vector<std::filesystem::path>> existing_clean_paths(const CleanPlan& plan) {
            std::vector<std::filesystem::path> paths;
            for (const std::filesystem::path& path: plan.paths()) {
                std::error_code failure;
                const bool exists = std::filesystem::exists(path, failure);
                if (failure) {
                    return std::unexpected(error("cannot inspect clean path `" + path.string() + "`: " + failure.message()));
                }
                if (exists)
                    paths.push_back(path);
            }
            for (const GeneratedCleanFile& generated: plan.generated_files()) {
                std::error_code failure;
                const bool exists = std::filesystem::exists(generated.path, failure);
                if (failure) {
                    return std::unexpected(error("cannot inspect generated file `" + generated.path.string() + "`: " + failure.message()));
                }
                if (exists)
                    paths.push_back(generated.path);
            }
            return paths;
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
                auto workspace = open_workspace(command.workspace);
                if (!workspace)
                    return fail(workspace.error());

                if (command.verbose) {
                    std::cout
                        << "manifest tree: "
                        << workspace->manifest_tree.documents
                        << " documents, "
                        << workspace->manifest_tree.packages
                        << " packages, "
                        << workspace->manifest_tree.package_sets
                        << " package sets, "
                        << workspace->manifest_tree.target_documents
                        << " target documents\n";
                }

                for (const PackageId root: workspace->graph.roots())
                    print_package(workspace->graph, root, 0, command.verbose);

                if (command.verbose && !workspace->instances.empty()) {
                    std::cout << "configured instances:\n";
                    for (const ConfiguredPackageInstance& instance: workspace->instances) {
                        std::cout << "  " << workspace->graph[instance.package].name << " -> " << instance.artifact;
                        if (!instance.contexts.empty()) {
                            std::cout << " [";
                            for (std::size_t index = 0; index < instance.contexts.size(); ++index) {
                                if (index != 0)
                                    std::cout << ", ";

                                std::cout << instance.contexts[index];
                            }
                            std::cout << ']';
                        }
                        std::cout << '\n';
                    }
                }

                return 0;
            }

            auto workspace = open_workspace(command.workspace);
            if (!workspace)
                return fail(workspace.error());

            if (command.mode == InspectMode::config) {
                const PackageNode& root = workspace->graph[workspace->graph.roots().front()];
                print_effective_configuration(
                    workspace->environment.configuration,
                    workspace->configuration_sources,
                    root.resolver,
                    workspace->environment.workspace,
                    command.verbose
                );
                print_providers(workspace->registry);
                return 0;
            }

            if (command.mode == InspectMode::targets) {
                auto inspected = inspect_effective_targets(workspace->graph, workspace->environment, command.verbose);
                return inspected ? 0 : fail(inspected.error());
            }

            auto plan = plan_build(workspace->graph, workspace->registry, workspace->environment);
            if (!plan)
                return fail(plan.error());

            if (command.mode == InspectMode::outputs) {
                inspect_outputs(workspace->graph, *plan, workspace->environment.workspace);
                return 0;
            }
            if (command.mode == InspectMode::actions) {
                auto printed = inspect_actions(workspace->graph, *plan, workspace->environment.workspace, command.verbose);
                return printed ? 0 : fail(printed.error());
            }

            auto state = check(*plan);
            if (!state)
                return fail(state.error());

            if (state->requires_synchronization()) {
                return fail(error("target information is not synchronized").add_note("run `kaixa generate` before inspecting targets"));
            }

            auto products = discover_products(workspace->graph, workspace->registry, workspace->environment);
            if (!products)
                return fail(products.error());

            print_products(*products, workspace->environment.workspace);
            return 0;
        }

        int run(const CheckCommand& command) {
            const DiagnosticFormat format = command.format;
            auto workspace = open_workspace(command.workspace);
            if (!workspace)
                return fail(workspace.error(), format);

            auto plan = plan_build(workspace->graph, workspace->registry, workspace->environment);
            if (!plan)
                return fail(plan.error(), format);

            auto report = check(*plan);
            if (!report)
                return fail(report.error(), format);

            bool reported = false;
            for (const GeneratedFileCheck& file: report->generated_files) {
                if (file.state == GeneratedFileState::current)
                    continue;

                if (format == DiagnosticFormat::short_form) {
                    if (!is_inside(file.path, workspace->environment.state_root)) {
                        std::cout
                            << file.path.string()
                            << ":1:1: warning: generated file is "
                            << state_name(file.state)
                            << "; run `kaixa generate`\n";
                        reported = true;
                    }
                    continue;
                }

                std::cout
                    << "generated file: "
                    << state_name(file.state)
                    << ' '
                    << display_path(file.path, workspace->environment.workspace)
                    << '\n';
            }
            if (format == DiagnosticFormat::short_form) {
                if (!report->requires_synchronization())
                    return 0;

                if (!reported) {
                    std::cout
                        << (workspace->environment.workspace / "Kaixa.toml").string()
                        << ":1:1: warning: workspace requires synchronization; run `kaixa generate`\n";
                }
                return 1;
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

            auto plan = plan_build(workspace->graph, workspace->registry, workspace->environment);
            if (!plan)
                return fail(plan.error());

            auto printed = print_actions(*plan, true);
            if (!printed)
                return fail(printed.error());

            auto report = generate(*plan);
            if (!report)
                return fail(report.error());

            std::cout
                << "workspace synchronized: "
                << report->written
                << " file(s) written, "
                << report->unchanged
                << " unchanged, "
                << report->synchronized
                << " action(s) run\n";
            return 0;
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
                    auto workspace = open_workspace(command.workspace, {}, false, false, false);
                    if (!workspace)
                        return fail(workspace.error());

                    auto generated = plan_clean(workspace->graph, workspace->registry, workspace->environment, CleanRequest{true});
                    if (!generated)
                        return fail(generated.error());

                    for (const GeneratedCleanFile& file: generated->generated_files())
                        plan.generated_file(file);
                }
            } else {
                auto workspace = open_workspace(command.workspace, {}, false, false, false);
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

            auto report = clean(plan, state_root, workspace_directory, command.dry_run, command.all);
            if (!report)
                return fail(report.error());

            if (existing->empty()) {
                std::cout << "nothing to clean\n";
                return 0;
            }

            for (const std::filesystem::path& path: *existing) {
                std::cout << (command.dry_run ? "would remove: " : "removed: ") << display_path(path, workspace_directory) << '\n';
            }
            if (!command.dry_run) {
                std::cout << "removed " << report->removed_entries << " filesystem entry(s)\n";
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

            const PackageNode& root = workspace->graph[workspace->graph.roots().front()];
            print_effective_configuration(
                workspace->environment.configuration,
                workspace->configuration_sources,
                root.resolver,
                workspace->environment.workspace,
                command.verbose
            );
            print_providers(workspace->registry);
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
        return std::visit([](const auto& value) { return detail::run(value); }, command);
    }
}
