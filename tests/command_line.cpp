#include <test_support.hpp>

#include <command_line.hpp>

#include <array>
#include <string_view>
#include <utility>
#include <variant>

KAIXA_TEST(command_line_build_keeps_workspace_options) {
    constexpr std::array arguments = {
        std::string_view("build"),
        std::string_view("--path"),
        std::string_view("project"),
        std::string_view("--package"),
        std::string_view("editor"),
        std::string_view("--package"),
        std::string_view("game_runner"),
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
    context.check_equal(command->workspace.packages.size(), std::size_t{2}, "selected package count");
    context.check_equal(command->workspace.packages.front(), std::string("editor"), "first selected package");
    context.check_equal(
        command->workspace.configurations.size(),
        std::size_t{1},
        "configuration count"
    );
}

KAIXA_TEST(command_line_can_replace_default_configurations) {
    constexpr std::array arguments = {
        std::string_view("build"),
        std::string_view("--no-default-configs"),
        std::string_view("--config"),
        std::string_view("clang-release")
    };
    const auto parsed = kaixa::cli::parse_command_line(arguments);
    context.check(parsed.has_value(), "explicit configuration replacement parses");
    if (!parsed)
        return;

    const auto* command = std::get_if<kaixa::cli::BuildCommand>(&*parsed);
    context.check(command != nullptr, "replacement keeps build command type");
    if (!command)
        return;

    context.check(!command->workspace.use_default_configurations, "configured defaults are disabled");
    context.check_equal(command->workspace.configurations.front(), std::string("clang-release"), "release config");
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

KAIXA_TEST(command_line_build_parses_semantic_product_selectors) {
    constexpr std::array arguments = {
        std::string_view("build"),
        std::string_view("--example"),
        std::string_view("hello_world"),
        std::string_view("--test"),
        std::string_view("unit_tests"),
        std::string_view("--bench"),
        std::string_view("allocation"),
        std::string_view("--list")
    };
    const auto parsed = kaixa::cli::parse_command_line(arguments);
    context.check(parsed.has_value(), "named product selectors parse");
    if (!parsed)
        return;

    const auto* command = std::get_if<kaixa::cli::BuildCommand>(&*parsed);
    context.check(command != nullptr, "product selection keeps build command type");
    if (!command)
        return;

    context.check_equal(command->selection.examples.front(), std::string("hello_world"), "selected example");
    context.check_equal(command->selection.tests.front(), std::string("unit_tests"), "selected test");
    context.check_equal(command->selection.benchmarks.front(), std::string("allocation"), "selected benchmark");
    context.check(command->list, "semantic selection composes with list");
}

KAIXA_TEST(command_line_build_rejects_conflicting_product_selectors) {
    constexpr std::array redundant_examples = {
        std::string_view("build"),
        std::string_view("--examples"),
        std::string_view("--example"),
        std::string_view("hello_world")
    };
    const auto examples = kaixa::cli::parse_command_line(redundant_examples);
    context.check(!examples.has_value(), "all and named examples conflict");
    if (!examples)
        context.check_contains(examples.error().message, "already selects every example", "example conflict reason");

    constexpr std::array all_targets = {
        std::string_view("build"),
        std::string_view("--all-targets"),
        std::string_view("--tests")
    };
    const auto all = kaixa::cli::parse_command_line(all_targets);
    context.check(!all.has_value(), "all targets and category conflict");
    if (!all)
        context.check_contains(all.error().message, "already selects every product", "all-targets conflict reason");

    constexpr std::array raw_target = {
        std::string_view("build"),
        std::string_view("--target"),
        std::string_view("hello_world"),
        std::string_view("--example"),
        std::string_view("hello_world")
    };
    const auto raw = kaixa::cli::parse_command_line(raw_target);
    context.check(!raw.has_value(), "raw and semantic target selection conflict");
    if (!raw)
        context.check_contains(raw.error().message, "semantic product selectors", "raw target conflict reason");
}

KAIXA_TEST(command_line_run_distinguishes_one_example_from_example_listing) {
    constexpr std::array selected_arguments = {
        std::string_view("run"),
        std::string_view("--example"),
        std::string_view("hello_world")
    };
    const auto selected = kaixa::cli::parse_command_line(selected_arguments);
    context.check(selected.has_value(), "named run example parses");
    if (selected) {
        const auto* command = std::get_if<kaixa::cli::RunCommand>(&*selected);
        context.check(command != nullptr, "named example keeps run command type");
        if (command)
            context.check_equal(command->example.value_or(""), std::string("hello_world"), "run example");
    }

    constexpr std::array listed_arguments = {
        std::string_view("run"),
        std::string_view("--examples"),
        std::string_view("--list")
    };
    const auto listed = kaixa::cli::parse_command_line(listed_arguments);
    context.check(listed.has_value(), "example listing parses");

    constexpr std::array invalid_arguments = {
        std::string_view("run"),
        std::string_view("--examples")
    };
    const auto invalid = kaixa::cli::parse_command_line(invalid_arguments);
    context.check(!invalid.has_value(), "running all examples is rejected");
    if (!invalid)
        context.check_contains(invalid.error().message, "valid only with --list", "run examples error explains fix");
}

KAIXA_TEST(command_line_inspect_targets_accepts_build_configuration) {
    constexpr std::array arguments = {
        std::string_view("inspect"),
        std::string_view("targets"),
        std::string_view("--path"),
        std::string_view("project"),
        std::string_view("--config"),
        std::string_view("clang")
    };
    const auto parsed = kaixa::cli::parse_command_line(arguments);
    context.check(parsed.has_value(), "inspect targets parses");
    if (!parsed)
        return;

    const auto* command = std::get_if<kaixa::cli::InspectCommand>(&*parsed);
    context.check(command != nullptr, "inspect command has its own type");
    if (command) {
        context.check(
            command->mode == kaixa::cli::InspectMode::targets,
            "inspect target mode is retained"
        );
        context.check_equal(
            command->workspace.path.generic_string(),
            std::string("project"),
            "inspect workspace path"
        );
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
        std::string_view("project")
    };
    const auto parsed = kaixa::cli::parse_command_line(arguments);
    context.check(!parsed.has_value(), "positional path is rejected");
    if (!parsed) {
        context.check_contains(
            parsed.error().message,
            "unexpected argument `project`",
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

KAIXA_TEST(command_line_bench_selects_a_target_and_forwards_arguments) {
    constexpr std::array arguments = {
        std::string_view("bench"),
        std::string_view("--target"),
        std::string_view("manifest_format"),
        std::string_view("--"),
        std::string_view("--iterations"),
        std::string_view("500")
    };
    const auto parsed = kaixa::cli::parse_command_line(arguments);
    context.check(parsed.has_value(), "bench command parses");
    if (!parsed)
        return;

    const auto* command = std::get_if<kaixa::cli::BenchCommand>(&*parsed);
    context.check(command != nullptr, "bench command has its own type");
    if (!command)
        return;

    context.check_equal(command->target.value_or(""), std::string("manifest_format"), "benchmark target");
    context.check_equal(command->arguments.size(), std::size_t{2}, "benchmark argument count");
}

KAIXA_TEST(command_line_bench_rejects_a_target_while_listing) {
    constexpr std::array arguments = {
        std::string_view("bench"),
        std::string_view("--list"),
        std::string_view("--target"),
        std::string_view("manifest_format")
    };
    const auto parsed = kaixa::cli::parse_command_line(arguments);
    context.check(!parsed.has_value(), "bench list rejects a selected target");
    if (parsed)
        return;

    context.check(
        parsed.error().message.find("omit --target to list benchmarks") != std::string::npos,
        "bench conflict explains the valid form"
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
        std::string_view("--path"),
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
        std::string_view("--verbose"),
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

    context.check(command->verbose, "config origin details are requested");
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

KAIXA_TEST(command_line_inspect_supports_every_mode) {
    for (const auto& [name, mode]: {
             std::pair{std::string_view("packages"), kaixa::cli::InspectMode::packages},
             std::pair{std::string_view("targets"), kaixa::cli::InspectMode::targets},
             std::pair{std::string_view("outputs"), kaixa::cli::InspectMode::outputs},
             std::pair{std::string_view("actions"), kaixa::cli::InspectMode::actions},
             std::pair{std::string_view("config"), kaixa::cli::InspectMode::config}
         }) {
        const std::array arguments = {
            std::string_view("inspect"),
            name,
            std::string_view("--verbose")
        };
        const auto parsed = kaixa::cli::parse_command_line(arguments);
        context.check(parsed.has_value(), std::string("inspect mode parses: ") + std::string(name));
        if (!parsed)
            continue;

        const auto* command = std::get_if<kaixa::cli::InspectCommand>(&*parsed);
        context.check(command != nullptr, "inspect mode keeps command type");
        if (command) {
            context.check(command->mode == mode, "inspect mode is retained");
            context.check(command->verbose, "inspect verbose mode is retained");
        }
    }
}

KAIXA_TEST(command_line_workspace_commands_require_the_path_option) {
    constexpr std::array check_arguments = {
        std::string_view("check"),
        std::string_view("--path"),
        std::string_view("project")
    };
    const auto checked = kaixa::cli::parse_command_line(check_arguments);
    context.check(checked.has_value(), "check accepts --path");
    if (checked) {
        const auto* command = std::get_if<kaixa::cli::CheckCommand>(&*checked);
        context.check(command != nullptr, "check keeps command type");
        if (command) {
            context.check_equal(
                command->workspace.path.generic_string(),
                std::string("project"),
                "check workspace path"
            );
        }
    }

    constexpr std::array positional_arguments = {
        std::string_view("generate"),
        std::string_view("project")
    };
    const auto positional = kaixa::cli::parse_command_line(positional_arguments);
    context.check(!positional.has_value(), "generate rejects a positional workspace path");
}
