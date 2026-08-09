#include <kaixa/kaixa.hpp>
#include <kaixa/foundation/process.hpp>
#include <kaixa/plugin/bundle.hpp>

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
    void print_usage() {
        std::cout
            << "Kaixa " << kaixa::version() << "\n\n"
            << "Usage:\n"
            << "  kaixa inspect [path]\n"
            << "  kaixa build [path] [--profile name] [-- <resolver arguments...>]\n"
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

    int build_workspace(
        const std::filesystem::path& path,
        std::string profile,
        std::vector<std::string> resolver_arguments
    ) {
        auto graph = kaixa::load_workspace(path);
        if (!graph)
            return fail(graph.error());

        const std::filesystem::path workspace = (*graph)[graph->root()].directory;
        const kaixa::BuildEnvironment environment{
            workspace,
            workspace / ".kaixa",
            std::move(profile),
            std::move(resolver_arguments)
        };
        const kaixa::ResolverRegistry registry = kaixa::plugin::default_registry();

        auto plan = kaixa::plan_build(*graph, registry, environment);
        if (!plan)
            return fail(plan.error());

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
    std::string profile = "debug";
    std::vector<std::string> resolver_arguments;
    bool has_path = false;
    for (int index = 2; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--") {
            for (++index; index < argc; ++index)
                resolver_arguments.emplace_back(argv[index]);
            break;
        }
        if (argument == "--profile") {
            if (++index == argc) {
                std::cerr << "error: --profile requires a value\n";
                return 2;
            }
            profile = argv[index];
            continue;
        }
        if (has_path) {
            std::cerr << "error: unexpected argument `" << argument << "`\n";
            return 2;
        }
        path = argument;
        has_path = true;
    }

    if (command == "inspect") {
        if (!resolver_arguments.empty()) {
            std::cerr << "error: resolver arguments are only valid with `build`\n";
            return 2;
        }
        return inspect_workspace(path);
    }
    if (command == "build")
        return build_workspace(
            path,
            std::move(profile),
            std::move(resolver_arguments)
        );

    std::cerr << "error: unknown command `" << command << "`\n";
    print_usage();
    return 2;
}
