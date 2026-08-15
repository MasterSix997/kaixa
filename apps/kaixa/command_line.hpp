#pragma once

#include <kaixa/config/build_configuration.hpp>
#include <kaixa/extension/resolver.hpp>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace kaixa::cli {
    struct WorkspaceOptions {
        std::filesystem::path path = ".";
        std::vector<std::string> packages;
        std::optional<std::string> profile;
        std::vector<std::string> configurations;
        std::vector<ResolverArgumentOverride> resolver_arguments;
        bool use_default_configurations = true;
    };

    struct HelpCommand {};
    struct VersionCommand {};

    enum class InspectMode {
        packages,
        targets,
        outputs,
        actions,
        config
    };

    struct InspectCommand {
        WorkspaceOptions workspace;
        InspectMode mode = InspectMode::packages;
        bool verbose = false;
    };

    struct CheckCommand {
        WorkspaceOptions workspace;
    };

    struct GenerateCommand {
        WorkspaceOptions workspace;
    };

    struct ProductSelection {
        std::vector<std::string> examples;
        std::vector<std::string> tests;
        std::vector<std::string> benchmarks;
        bool all_examples = false;
        bool all_tests = false;
        bool all_benchmarks = false;
        bool all_targets = false;

        [[nodiscard]] bool empty() const noexcept {
            return examples.empty() && tests.empty() && benchmarks.empty()
                && !all_examples && !all_tests && !all_benchmarks && !all_targets;
        }
    };

    struct BuildCommand {
        WorkspaceOptions workspace;
        std::vector<std::string> targets;
        ProductSelection selection;
        std::optional<std::size_t> jobs;
        bool list = false;
    };

    struct TestCommand {
        WorkspaceOptions workspace;
        TestRequest request;
    };

    struct BenchCommand {
        WorkspaceOptions workspace;
        std::optional<std::string> target;
        std::vector<std::string> arguments;
        bool list = false;
    };

    struct RunCommand {
        WorkspaceOptions workspace;
        std::optional<std::string> target;
        std::optional<std::string> example;
        std::vector<std::string> arguments;
        bool list = false;
        bool examples = false;
    };

    struct CleanCommand {
        WorkspaceOptions workspace;
        bool all = false;
        bool generated_files = false;
        bool dry_run = false;
    };

    struct ConfigListCommand {
        std::filesystem::path path = ".";
    };

    struct ConfigShowCommand {
        WorkspaceOptions workspace;
        bool verbose = false;
    };

    struct ConfigPathCommand {
        std::filesystem::path path = ".";
    };

    using Command = std::variant<
        HelpCommand,
        VersionCommand,
        InspectCommand,
        CheckCommand,
        GenerateCommand,
        BuildCommand,
        TestCommand,
        BenchCommand,
        RunCommand,
        CleanCommand,
        ConfigListCommand,
        ConfigShowCommand,
        ConfigPathCommand
    >;

    struct ParseError {
        std::string message;
        bool show_usage = false;
    };

    void print_usage(std::ostream& out);
    [[nodiscard]] std::expected<Command, ParseError> parse_command_line(std::span<const std::string_view> arguments);
}
