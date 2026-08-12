#include <kaixa/kaixa.hpp>
#include <kaixa/foundation/process.hpp>
#include <kaixa/plugin/bundle.hpp>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {
    void print_usage() {
        std::cout
            << "Kaixa " << kaixa::version() << "\n\n"
            << "Usage:\n"
            << "  kaixa inspect [path]\n"
            << "  kaixa <check|generate|build> [path] [--profile name] [--config name]...\n"
            << "        [--for resolver <arguments...>]...\n"
            << "  kaixa --version\n";
    }

    int fail(const kaixa::Diagnostic& diagnostic) {
        std::cerr << kaixa::format_diagnostic(diagnostic) << '\n';
        return 1;
    }

    void print_package(const kaixa::Graph& graph, const kaixa::PackageId id, const int depth) {
        const kaixa::PackageNode& package = graph[id];
        std::cout << std::string(static_cast<std::size_t>(depth) * 2, ' ')
                  << package.name;
        if (package.kind == kaixa::PackageKind::opaque)
            std::cout << " (opaque)";
        else
            std::cout << " (" << package.resolver << ')';
        std::cout << '\n';

        for (const kaixa::PackageId dependency: package.dependencies)
            print_package(graph, dependency, depth + 1);
    }

    int inspect_workspace(const std::filesystem::path& path) {
        auto graph = kaixa::load_workspace(path);
        if (!graph)
            return fail(graph.error());
        print_package(*graph, graph->root(), 0);
        return 0;
    }

    std::optional<std::filesystem::path> user_configuration_path() {
#ifdef _WIN32
        const std::optional<std::string> base = kaixa::environment_variable("APPDATA");
        if (base)
            return std::filesystem::path(*base) / "Kaixa" / "config.toml";
#else
        const std::optional<std::string> xdg = kaixa::environment_variable("XDG_CONFIG_HOME");
        if (xdg)
            return std::filesystem::path(*xdg) / "kaixa" / "config.toml";
        const std::optional<std::string> home = kaixa::environment_variable("HOME");
        if (home)
            return std::filesystem::path(*home) / ".config" / "kaixa" / "config.toml";
#endif
        return std::nullopt;
    }

    kaixa::Result<void> append_configuration_file(
        std::vector<kaixa::ConfigurationSet>& layers,
        const std::filesystem::path& path
    ) {
        std::error_code failure;
        const bool exists = std::filesystem::exists(path, failure);
        if (failure == std::make_error_code(std::errc::no_such_file_or_directory))
            return {};
        if (failure) {
            return std::unexpected(kaixa::error(
                "cannot inspect configuration file `" + path.string() + "`: "
                    + failure.message()
            ));
        }
        if (!exists)
            return {};
        if (!std::filesystem::is_regular_file(path, failure) || failure) {
            return std::unexpected(kaixa::error(
                "configuration path is not a regular file: " + path.string()
            ));
        }

        auto configuration = kaixa::parse_configuration_file(path);
        if (!configuration)
            return std::unexpected(configuration.error());
        layers.push_back(std::move(*configuration));
        return {};
    }

    bool resolver_is_active(const kaixa::Graph& graph, const std::string_view resolver) {
        return std::ranges::any_of(graph.nodes(), [&](const kaixa::PackageNode& package) {
            return package.kind == kaixa::PackageKind::managed && package.resolver == resolver;
        });
    }

    enum class WorkspaceOperation {
        check,
        generate,
        build
    };

    std::string_view state_name(const kaixa::GeneratedFileState state) {
        switch (state) {
            case kaixa::GeneratedFileState::current: return "current";
            case kaixa::GeneratedFileState::missing: return "missing";
            case kaixa::GeneratedFileState::different: return "different";
        }
        return "unknown";
    }

    std::string_view state_name(const kaixa::ActionState state) {
        switch (state) {
            case kaixa::ActionState::current: return "current";
            case kaixa::ActionState::required: return "required";
            case kaixa::ActionState::unknown: return "unknown";
        }
        return "unknown";
    }

    std::string_view stage_name(const kaixa::ActionStage stage) {
        switch (stage) {
            case kaixa::ActionStage::synchronize: return "synchronization";
            case kaixa::ActionStage::build: return "build";
        }
        return "action";
    }

    int process_workspace(
        const WorkspaceOperation operation,
        const std::filesystem::path& path,
        std::optional<std::string> profile,
        std::vector<std::string> configurations,
        std::vector<kaixa::ResolverArgumentOverride> resolver_arguments
    ) {
        auto graph = kaixa::load_workspace(path);
        if (!graph)
            return fail(graph.error());

        const std::filesystem::path workspace = (*graph)[graph->root()].directory;
        for (const kaixa::ResolverArgumentOverride& override: resolver_arguments) {
            if (!resolver_is_active(*graph, override.resolver)) {
                return fail(kaixa::error(
                    "resolver `" + override.resolver + "` does not participate in this build"
                ));
            }
        }

        std::vector<kaixa::ConfigurationSet> layers;
        const kaixa::PackageNode& root = (*graph)[graph->root()];
        if (root.manifest)
            layers.push_back(root.manifest->configurations);

        if (const auto user = user_configuration_path()) {
            auto loaded = append_configuration_file(layers, *user);
            if (!loaded)
                return fail(loaded.error());
        }
        auto local = append_configuration_file(layers, workspace / "Kaixa.user.toml");
        if (!local)
            return fail(local.error());

        auto effective = kaixa::resolve_configurations(
            layers,
            configurations,
            profile,
            resolver_arguments
        );
        if (!effective)
            return fail(effective.error());

        const kaixa::BuildEnvironment environment{
            workspace,
            workspace / ".kaixa",
            std::move(*effective)
        };
        const kaixa::ResolverRegistry registry = kaixa::plugin::default_registry();

        auto plan = kaixa::plan_build(*graph, registry, environment);
        if (!plan)
            return fail(plan.error());

        if (operation == WorkspaceOperation::check) {
            auto report = kaixa::check(*plan);
            if (!report)
                return fail(report.error());

            for (const kaixa::GeneratedFileCheck& file: report->generated_files)
                std::cout << state_name(file.state) << " generated file: " << file.path.string() << '\n';

            for (const kaixa::ActionCheck& action: report->actions) {
                std::cout << state_name(action.state) << ' ' << stage_name(action.stage) << ": " << action.description << '\n';
            }

            if (report->requires_synchronization()) {
                std::cout << "synchronization required\n";
                return 1;
            }
            std::cout << "workspace synchronized\n";
            return 0;
        }

        if (operation == WorkspaceOperation::generate) {
            for (const kaixa::Action& action: plan->actions()) {
                if (action.stage == kaixa::ActionStage::synchronize)
                    std::cout << action.description << ": " << kaixa::format_command(action.argv) << '\n';
            }
            std::cout.flush();

            auto report = kaixa::generate(*plan);
            if (!report)
                return fail(report.error());

            for (const kaixa::GeneratedFile& file: plan->generated_files())
                std::cout << "output file: " << file.path.string() << '\n';

            std::cout << "wrote " << report->written << " file(s); " << report->unchanged
                      << " unchanged; synchronized " << report->synchronized << " action(s)\n";
            return 0;
        }

        for (const kaixa::Action& action: plan->actions())
            std::cout << action.description << ": " << kaixa::format_command(action.argv) << '\n';
        std::cout.flush();

        auto report = kaixa::execute(*plan);
        if (!report)
            return fail(report.error());
        std::cout << "completed " << report->executed << " action(s)\n";
        return 0;
    }
}

int main(const int argc, char** argv) {
    if (argc == 1) {
        print_usage();
        return 0;
    }

    const std::string_view command = argv[1];
    if (command == "--version") {
        std::cout << "kaixa " << kaixa::version() << '\n';
        return 0;
    }
    if (command == "--help" || command == "-h") {
        print_usage();
        return 0;
    }

    std::filesystem::path path = ".";
    std::optional<std::string> profile;
    std::vector<std::string> configurations;
    std::vector<kaixa::ResolverArgumentOverride> resolver_arguments;
    bool has_path = false;
    for (int index = 2; index < argc;) {
        const std::string_view argument = argv[index];
        if (argument == "--profile") {
            if (++index == argc) {
                std::cerr << "error: --profile requires a value\n";
                return 2;
            }
            profile = argv[index];
            ++index;
            continue;
        }
        if (argument == "--config") {
            if (++index == argc) {
                std::cerr << "error: --config requires a value\n";
                return 2;
            }
            configurations.emplace_back(argv[index]);
            ++index;
            continue;
        }
        if (argument == "--for") {
            if (++index == argc) {
                std::cerr << "error: --for requires a resolver name\n";
                return 2;
            }
            const std::string resolver = argv[index++];
            std::vector<std::string> arguments;
            while (index < argc && std::string_view(argv[index]) != "--for")
                arguments.emplace_back(argv[index++]);
            if (arguments.empty()) {
                std::cerr << "error: --for " << resolver << " requires arguments\n";
                return 2;
            }

            const auto existing = std::ranges::find_if(
                resolver_arguments,
                [&](const kaixa::ResolverArgumentOverride& item) {
                    return item.resolver == resolver;
                }
            );
            if (existing == resolver_arguments.end()) {
                resolver_arguments.push_back({resolver, std::move(arguments)});
            } else {
                existing->arguments.insert(
                    existing->arguments.end(),
                    arguments.begin(),
                    arguments.end()
                );
            }
            continue;
        }
        if (has_path) {
            std::cerr << "error: unexpected argument `" << argument << "`\n";
            return 2;
        }
        path = argument;
        has_path = true;
        ++index;
    }

    if (command == "inspect") {
        if (!resolver_arguments.empty() || !configurations.empty() || profile) {
            std::cerr << "error: build options are not valid with `inspect`\n";
            return 2;
        }
        return inspect_workspace(path);
    }

    std::optional<WorkspaceOperation> operation;
    if (command == "check")
        operation = WorkspaceOperation::check;
    else if (command == "generate")
        operation = WorkspaceOperation::generate;
    else if (command == "build")
        operation = WorkspaceOperation::build;

    if (operation) {
        return process_workspace(
            *operation,
            path,
            std::move(profile),
            std::move(configurations),
            std::move(resolver_arguments)
        );
    }

    std::cerr << "error: unknown command `" << command << "`\n";
    print_usage();
    return 2;
}
