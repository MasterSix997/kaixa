#include <test_support.hpp>

#include <command_line.hpp>

#include <array>
#include <string_view>
#include <variant>

KAIXA_TEST(command_line_build_has_only_workspace_options) {
    constexpr std::array arguments = {
        std::string_view("build"),
        std::string_view("project"),
        std::string_view("--profile"),
        std::string_view("release"),
        std::string_view("--config"),
        std::string_view("clang")
    };
    const auto parsed = kaixa::cli::parse_command_line(arguments);
    context.check(parsed.has_value(), "build command parses");
    if (!parsed)
        return;

    const auto* command = std::get_if<kaixa::cli::BuildCommand>(&*parsed);
    context.check(command != nullptr, "build command has its own type");
    if (!command)
        return;

    context.check_equal(
        command->workspace.path.generic_string(),
        std::string("project"),
        "workspace path"
    );
    context.check_equal(
        command->workspace.profile.value_or(""),
        std::string("release"),
        "profile"
    );
    context.check_equal(
        command->workspace.configurations.size(),
        std::size_t{1},
        "configuration count"
    );
}

KAIXA_TEST(command_line_test_keeps_test_options_together) {
    constexpr std::array arguments = {
        std::string_view("test"),
        std::string_view("manifest"),
        std::string_view("--target"),
        std::string_view("kaixa_tests"),
        std::string_view("--path"),
        std::string_view("project")
    };
    const auto parsed = kaixa::cli::parse_command_line(arguments);
    context.check(parsed.has_value(), "test command parses");
    if (!parsed)
        return;

    const auto* command = std::get_if<kaixa::cli::TestCommand>(&*parsed);
    context.check(command != nullptr, "test command has its own type");
    if (!command)
        return;

    context.check_equal(
        command->request.filter.value_or(""),
        std::string("manifest"),
        "test filter"
    );
    context.check_equal(
        command->request.target.value_or(""),
        std::string("kaixa_tests"),
        "test target"
    );
    context.check_equal(
        command->workspace.path.generic_string(),
        std::string("project"),
        "test workspace path"
    );
}

KAIXA_TEST(command_line_rejects_extra_positional_arguments) {
    constexpr std::array arguments = {
        std::string_view("build"),
        std::string_view("first"),
        std::string_view("second")
    };
    const auto parsed = kaixa::cli::parse_command_line(arguments);
    context.check(!parsed.has_value(), "extra path is rejected");
    if (!parsed) {
        context.check_contains(
            parsed.error().message,
            "unexpected argument `second`",
            "specific parsing error"
        );
    }
}

KAIXA_TEST(command_line_test_list_composes_with_filter_and_target) {
    constexpr std::array arguments = {
        std::string_view("test"),
        std::string_view("manifest"),
        std::string_view("--list"),
        std::string_view("--target"),
        std::string_view("kaixa_tests")
    };
    const auto parsed = kaixa::cli::parse_command_line(arguments);
    context.check(parsed.has_value(), "test list command parses");
    if (!parsed)
        return;

    const auto* command = std::get_if<kaixa::cli::TestCommand>(&*parsed);
    context.check(command != nullptr, "test list keeps the test command type");
    if (!command)
        return;

    context.check(
        command->request.mode == kaixa::TestMode::list,
        "test request uses list mode"
    );
    context.check_equal(
        command->request.filter.value_or(""),
        std::string("manifest"),
        "list filter"
    );
    context.check_equal(
        command->request.target.value_or(""),
        std::string("kaixa_tests"),
        "list target"
    );
}

KAIXA_TEST(command_line_run_separates_program_arguments) {
    constexpr std::array arguments = {
        std::string_view("run"),
        std::string_view("--target"),
        std::string_view("editor"),
        std::string_view("--config"),
        std::string_view("clang"),
        std::string_view("--"),
        std::string_view("--project"),
        std::string_view("sandbox")
    };
    const auto parsed = kaixa::cli::parse_command_line(arguments);
    context.check(parsed.has_value(), "run command parses");
    if (!parsed)
        return;

    const auto* command = std::get_if<kaixa::cli::RunCommand>(&*parsed);
    context.check(command != nullptr, "run command has its own type");
    if (!command)
        return;

    context.check_equal(command->target.value_or(""), std::string("editor"), "run target");
    context.check_equal(command->arguments.size(), std::size_t{2}, "program argument count");
    context.check_equal(command->arguments.front(), std::string("--project"), "first argument");
    context.check_equal(
        command->workspace.configurations.front(),
        std::string("clang"),
        "run configuration"
    );
}

KAIXA_TEST(command_line_run_list_is_explicit) {
    constexpr std::array arguments = {
        std::string_view("run"),
        std::string_view("--list"),
        std::string_view("--path"),
        std::string_view("project")
    };
    const auto parsed = kaixa::cli::parse_command_line(arguments);
    context.check(parsed.has_value(), "run list parses");
    if (!parsed)
        return;

    const auto* command = std::get_if<kaixa::cli::RunCommand>(&*parsed);
    context.check(command != nullptr && command->list, "run list mode is retained");
    if (command) {
        context.check_equal(
            command->workspace.path.generic_string(),
            std::string("project"),
            "run list workspace"
        );
    }
}

KAIXA_TEST(command_line_clean_supports_dry_run_and_all) {
    constexpr std::array arguments = {
        std::string_view("clean"),
        std::string_view("project"),
        std::string_view("--all"),
        std::string_view("--generated-files"),
        std::string_view("--dry-run")
    };
    const auto parsed = kaixa::cli::parse_command_line(arguments);
    context.check(parsed.has_value(), "clean all parses");
    if (!parsed)
        return;

    const auto* command = std::get_if<kaixa::cli::CleanCommand>(&*parsed);
    context.check(command != nullptr, "clean command has its own type");
    if (!command)
        return;

    context.check(command->all, "clean all mode");
    context.check(command->generated_files, "clean generated files mode");
    context.check(command->dry_run, "clean dry run mode");
    context.check_equal(
        command->workspace.path.generic_string(),
        std::string("project"),
        "clean workspace"
    );
}

KAIXA_TEST(command_line_clean_all_rejects_configuration_selection) {
    constexpr std::array arguments = {
        std::string_view("clean"),
        std::string_view("--all"),
        std::string_view("--config"),
        std::string_view("clang")
    };
    const auto parsed = kaixa::cli::parse_command_line(arguments);
    context.check(!parsed.has_value(), "clean all rejects configuration");
    if (!parsed) {
        context.check_contains(
            parsed.error().message,
            "cannot be combined",
            "clean all error"
        );
    }
}
