#include <test_support.hpp>

#include <command_line.hpp>

#include <array>
#include <string_view>
#include <variant>

KAIXA_TEST(command_line_build_keeps_workspace_options) {
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

KAIXA_TEST(command_line_build_lists_or_selects_multiple_targets) {
    constexpr std::array selected_arguments = {
        std::string_view("build"),
        std::string_view("--target"),
        std::string_view("core"),
        std::string_view("--target"),
        std::string_view("app"),
        std::string_view("--jobs"),
        std::string_view("6"),
        std::string_view("--for"),
        std::string_view("cmake.configure"),
        std::string_view("-DDEV=ON"),
        std::string_view("--for"),
        std::string_view("cmake.build"),
        std::string_view("--verbose")
    };
    const auto selected = kaixa::cli::parse_command_line(selected_arguments);
    context.check(selected.has_value(), "multiple build targets parse");
    if (selected) {
        const auto* command = std::get_if<kaixa::cli::BuildCommand>(&*selected);
        context.check(command != nullptr, "selected build keeps build command type");
        if (command) {
            context.check_equal(command->targets.size(), std::size_t{2}, "build target count");
            context.check_equal(command->targets.front(), std::string("core"), "first build target");
            context.check_equal(command->jobs.value_or(0), std::size_t{6}, "build job count");
            context.check_equal(
                command->workspace.resolver_arguments.size(),
                std::size_t{2},
                "resolver argument scope count"
            );
            context.check_equal(
                command->workspace.resolver_arguments.front().scope,
                std::string("configure"),
                "configure argument scope"
            );
            context.check_equal(
                command->workspace.resolver_arguments.back().scope,
                std::string("build"),
                "build argument scope"
            );
        }
    }

    constexpr std::array list_arguments = {
        std::string_view("build"),
        std::string_view("--list")
    };
    const auto listed = kaixa::cli::parse_command_line(list_arguments);
    context.check(listed.has_value(), "build list parses");
    if (listed) {
        const auto* command = std::get_if<kaixa::cli::BuildCommand>(&*listed);
        context.check(command != nullptr && command->list, "build list mode is retained");
    }
}

KAIXA_TEST(command_line_build_rejects_invalid_job_counts) {
    constexpr std::array arguments = {
        std::string_view("build"),
        std::string_view("--jobs"),
        std::string_view("0")
    };
    const auto parsed = kaixa::cli::parse_command_line(arguments);
    context.check(!parsed.has_value(), "zero jobs are rejected");
    if (!parsed) {
        context.check_contains(
            parsed.error().message,
            "positive integer",
            "job count error explains requirement"
        );
    }
}

KAIXA_TEST(command_line_inspect_targets_accepts_build_configuration) {
    constexpr std::array arguments = {
        std::string_view("inspect"),
        std::string_view("project"),
        std::string_view("--targets"),
        std::string_view("--config"),
        std::string_view("clang")
    };
    const auto parsed = kaixa::cli::parse_command_line(arguments);
    context.check(parsed.has_value(), "inspect targets parses");
    if (!parsed)
        return;

    const auto* command = std::get_if<kaixa::cli::InspectCommand>(&*parsed);
    context.check(command != nullptr && command->targets, "inspect target mode is retained");
    if (command) {
        context.check_equal(
            command->workspace.configurations.front(),
            std::string("clang"),
            "inspect configuration"
        );
    }
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

KAIXA_TEST(command_line_config_list_and_path_accept_workspace_paths) {
    constexpr std::array list_arguments = {
        std::string_view("config"),
        std::string_view("list"),
        std::string_view("--path"),
        std::string_view("project")
    };
    const auto listed = kaixa::cli::parse_command_line(list_arguments);
    context.check(listed.has_value(), "config list parses");
    if (listed) {
        const auto* command = std::get_if<kaixa::cli::ConfigListCommand>(&*listed);
        context.check(command != nullptr, "config list has its own type");
        if (command) {
            context.check_equal(
                command->path.generic_string(),
                std::string("project"),
                "config list workspace"
            );
        }
    }

    constexpr std::array path_arguments = {
        std::string_view("config"),
        std::string_view("path"),
        std::string_view("--path"),
        std::string_view("project")
    };
    const auto path = kaixa::cli::parse_command_line(path_arguments);
    context.check(path.has_value(), "config path parses");
    if (path) {
        const auto* command = std::get_if<kaixa::cli::ConfigPathCommand>(&*path);
        context.check(command != nullptr, "config path has its own type");
    }
}

KAIXA_TEST(command_line_config_show_composes_named_config_and_overrides) {
    constexpr std::array arguments = {
        std::string_view("config"),
        std::string_view("show"),
        std::string_view("clang"),
        std::string_view("--profile"),
        std::string_view("release"),
        std::string_view("--for"),
        std::string_view("cmake.configure"),
        std::string_view("-DDEV=ON")
    };
    const auto parsed = kaixa::cli::parse_command_line(arguments);
    context.check(parsed.has_value(), "config show parses");
    if (!parsed)
        return;

    const auto* command = std::get_if<kaixa::cli::ConfigShowCommand>(&*parsed);
    context.check(command != nullptr, "config show has its own type");
    if (!command)
        return;

    context.check_equal(
        command->workspace.configurations.front(),
        std::string("clang"),
        "named config selection"
    );
    context.check_equal(
        command->workspace.profile.value_or(""),
        std::string("release"),
        "shown profile override"
    );
    context.check_equal(
        command->workspace.resolver_arguments.front().scope,
        std::string("configure"),
        "shown resolver scope"
    );
}
