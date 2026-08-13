#include "command_line.hpp"

#include <kaixa/kaixa.hpp>

#include <algorithm>
#include <ostream>
#include <utility>

namespace kaixa::cli {
    namespace {
        class Parser {
        public:
            explicit Parser(const std::span<const std::string_view> arguments)
                : m_arguments(arguments) {
            }

            [[nodiscard]] bool done() const noexcept {
                return m_index == m_arguments.size();
            }

            [[nodiscard]] std::string_view peek() const {
                return m_arguments[m_index];
            }

            std::string_view take() {
                return m_arguments[m_index++];
            }

            [[nodiscard]] std::expected<std::string_view, ParseError> value(const std::string_view option) {
                if (done()) {
                    return std::unexpected(ParseError{
                        std::string(option) + " requires a value"
                    });
                }

                return take();
            }

        private:
            std::span<const std::string_view> m_arguments;
            std::size_t m_index = 0;
        };

        void append_resolver_arguments(
            WorkspaceOptions& options,
            std::string resolver,
            std::vector<std::string> arguments
        ) {
            const auto existing = std::ranges::find_if(
                options.resolver_arguments,
                [&](const ResolverArgumentOverride& item) {
                    return item.resolver == resolver;
                }
            );
            if (existing == options.resolver_arguments.end()) {
                options.resolver_arguments.push_back({
                    std::move(resolver),
                    std::move(arguments)
                });
                return;
            }

            existing->arguments.insert(
                existing->arguments.end(),
                arguments.begin(),
                arguments.end()
            );
        }

        std::expected<bool, ParseError> parse_workspace_option(Parser& parser, WorkspaceOptions& options) {
            const std::string_view option = parser.peek();
            if (option == "--profile") {
                parser.take();
                auto value = parser.value(option);
                if (!value)
                    return std::unexpected(value.error());

                options.profile = *value;
                return true;
            }

            if (option == "--config") {
                parser.take();
                auto value = parser.value(option);
                if (!value)
                    return std::unexpected(value.error());

                options.configurations.emplace_back(*value);
                return true;
            }

            if (option != "--for")
                return false;

            parser.take();
            auto resolver = parser.value("--for");
            if (!resolver) {
                return std::unexpected(ParseError{
                    "--for requires a resolver name"
                });
            }

            std::vector<std::string> arguments;
            while (!parser.done() && parser.peek() != "--for" && parser.peek() != "--")
                arguments.emplace_back(parser.take());

            if (arguments.empty()) {
                return std::unexpected(ParseError{
                    "--for " + std::string(*resolver) + " requires arguments"
                });
            }

            append_resolver_arguments(options, std::string(*resolver), std::move(arguments));
            return true;
        }

        std::expected<WorkspaceOptions, ParseError> parse_workspace(Parser& parser) {
            WorkspaceOptions options;
            bool has_path = false;
            while (!parser.done()) {
                auto parsed = parse_workspace_option(parser, options);
                if (!parsed)
                    return std::unexpected(parsed.error());
                if (*parsed)
                    continue;

                const std::string_view argument = parser.take();
                if (has_path) {
                    return std::unexpected(ParseError{
                        "unexpected argument `" + std::string(argument) + "`"
                    });
                }

                options.path = argument;
                has_path = true;
            }
            return options;
        }

        std::expected<TestCommand, ParseError> parse_test(Parser& parser) {
            TestCommand command;
            while (!parser.done()) {
                const std::string_view argument = parser.peek();
                if (argument == "--path") {
                    parser.take();
                    auto value = parser.value(argument);
                    if (!value)
                        return std::unexpected(value.error());

                    command.workspace.path = *value;
                    continue;
                }

                if (argument == "--target") {
                    parser.take();
                    auto value = parser.value(argument);
                    if (!value)
                        return std::unexpected(value.error());

                    command.request.target = *value;
                    continue;
                }

                if (argument == "--list") {
                    parser.take();
                    command.request.mode = TestMode::list;
                    continue;
                }

                auto parsed = parse_workspace_option(parser, command.workspace);
                if (!parsed)
                    return std::unexpected(parsed.error());
                if (*parsed)
                    continue;

                parser.take();
                if (command.request.filter) {
                    return std::unexpected(ParseError{
                        "unexpected argument `" + std::string(argument) + "`"
                    });
                }

                command.request.filter = argument;
            }
            return command;
        }

        std::expected<RunCommand, ParseError> parse_run(Parser& parser) {
            RunCommand command;
            while (!parser.done()) {
                const std::string_view argument = parser.peek();
                if (argument == "--") {
                    parser.take();
                    while (!parser.done())
                        command.arguments.emplace_back(parser.take());

                    break;
                }

                if (argument == "--path") {
                    parser.take();
                    auto value = parser.value(argument);
                    if (!value)
                        return std::unexpected(value.error());

                    command.workspace.path = *value;
                    continue;
                }

                if (argument == "--target") {
                    parser.take();
                    auto value = parser.value(argument);
                    if (!value)
                        return std::unexpected(value.error());

                    command.target = *value;
                    continue;
                }

                if (argument == "--list") {
                    parser.take();
                    command.list = true;
                    continue;
                }

                auto parsed = parse_workspace_option(parser, command.workspace);
                if (!parsed)
                    return std::unexpected(parsed.error());
                if (*parsed)
                    continue;

                return std::unexpected(ParseError{
                    "unexpected argument `" + std::string(parser.take()) + "`"
                });
            }

            if (command.list && command.target) {
                return std::unexpected(ParseError{
                    "--list cannot be combined with --target"
                });
            }
            if (command.list && !command.arguments.empty()) {
                return std::unexpected(ParseError{
                    "--list cannot be combined with program arguments"
                });
            }
            return command;
        }

        std::expected<CleanCommand, ParseError> parse_clean(Parser& parser) {
            CleanCommand command;
            bool has_path = false;
            while (!parser.done()) {
                const std::string_view argument = parser.peek();
                if (argument == "--all") {
                    parser.take();
                    command.all = true;
                    continue;
                }
                if (argument == "--generated-files") {
                    parser.take();
                    command.generated_files = true;
                    continue;
                }
                if (argument == "--dry-run") {
                    parser.take();
                    command.dry_run = true;
                    continue;
                }

                auto parsed = parse_workspace_option(parser, command.workspace);
                if (!parsed)
                    return std::unexpected(parsed.error());
                if (*parsed)
                    continue;

                parser.take();
                if (has_path) {
                    return std::unexpected(ParseError{
                        "unexpected argument `" + std::string(argument) + "`"
                    });
                }

                command.workspace.path = argument;
                has_path = true;
            }

            if (command.all && (command.workspace.profile
                || !command.workspace.configurations.empty()
                || !command.workspace.resolver_arguments.empty())) {
                return std::unexpected(ParseError{
                    "--all cannot be combined with build configuration options"
                });
            }
            return command;
        }

        std::expected<InspectCommand, ParseError> parse_inspect(Parser& parser) {
            InspectCommand command;
            if (!parser.done())
                command.path = parser.take();

            if (!parser.done()) {
                return std::unexpected(ParseError{
                    "unexpected argument `" + std::string(parser.take()) + "`"
                });
            }
            return command;
        }
    }

    void print_usage(std::ostream& out) {
        out
            << "Kaixa " << version() << "\n\n"
            << "Usage:\n"
            << "  kaixa inspect [path]\n"
            << "  kaixa <check|generate|build> [path] [--profile name] [--config name]...\n"
            << "        [--for resolver <arguments...>]...\n"
            << "  kaixa test [filter] [--list] [--target name] [--path path] [--profile name]\n"
            << "        [--config name]...\n"
            << "        [--for resolver <arguments...>]...\n"
            << "  kaixa run [--list] [--target name] [--path path] [--profile name]\n"
            << "        [--config name]... [--for resolver <arguments...>]... [-- <arguments...>]\n"
            << "  kaixa clean [path] [--profile name] [--config name]...\n"
            << "        [--for resolver <arguments...>]... [--generated-files] [--dry-run]\n"
            << "  kaixa clean [path] --all [--generated-files] [--dry-run]\n"
            << "  kaixa --version\n";
    }

    std::expected<Command, ParseError> parse_command_line(const std::span<const std::string_view> arguments) {
        if (arguments.empty())
            return HelpCommand{};

        Parser parser(arguments);
        const std::string_view name = parser.take();
        if (name == "--help" || name == "-h")
            return HelpCommand{};
        if (name == "--version")
            return VersionCommand{};

        if (name == "inspect") {
            auto command = parse_inspect(parser);
            if (!command)
                return std::unexpected(command.error());

            return Command{std::move(*command)};
        }

        if (name == "test") {
            auto command = parse_test(parser);
            if (!command)
                return std::unexpected(command.error());

            return Command{std::move(*command)};
        }

        if (name == "run") {
            auto command = parse_run(parser);
            if (!command)
                return std::unexpected(command.error());

            return Command{std::move(*command)};
        }

        if (name == "clean") {
            auto command = parse_clean(parser);
            if (!command)
                return std::unexpected(command.error());

            return Command{std::move(*command)};
        }

        if (name != "check" && name != "generate" && name != "build") {
            return std::unexpected(ParseError{
                "unknown command `" + std::string(name) + "`",
                true
            });
        }

        auto workspace = parse_workspace(parser);
        if (!workspace)
            return std::unexpected(workspace.error());

        if (name == "check")
            return Command{CheckCommand{std::move(*workspace)}};
        if (name == "generate")
            return Command{GenerateCommand{std::move(*workspace)}};

        return Command{BuildCommand{std::move(*workspace)}};
    }
}
