#pragma once

#include <kaixa/config/build_configuration.hpp>
#include <kaixa/extension/resolver.hpp>

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
        std::optional<std::string> profile;
        std::vector<std::string> configurations;
        std::vector<ResolverArgumentOverride> resolver_arguments;
    };

    struct HelpCommand {};
    struct VersionCommand {};

    struct InspectCommand {
        std::filesystem::path path = ".";
    };

    struct CheckCommand {
        WorkspaceOptions workspace;
    };

    struct GenerateCommand {
        WorkspaceOptions workspace;
    };

    struct BuildCommand {
        WorkspaceOptions workspace;
    };

    struct TestCommand {
        WorkspaceOptions workspace;
        TestRequest request;
    };

    struct RunCommand {
        WorkspaceOptions workspace;
        std::optional<std::string> target;
        std::vector<std::string> arguments;
        bool list = false;
    };

    struct CleanCommand {
        WorkspaceOptions workspace;
        bool all = false;
        bool generated_files = false;
        bool dry_run = false;
    };

    using Command = std::variant<
        HelpCommand,
        VersionCommand,
        InspectCommand,
        CheckCommand,
        GenerateCommand,
        BuildCommand,
        TestCommand,
        RunCommand,
        CleanCommand
    >;

    struct ParseError {
        std::string message;
        bool show_usage = false;
    };

    void print_usage(std::ostream& out);
    [[nodiscard]] std::expected<Command, ParseError> parse_command_line(std::span<const std::string_view> arguments);
}
