#include <kaixa/kaixa.hpp>
#include <test_support.hpp>

#include <string>
#include <vector>

namespace {
    kaixa::Result<std::vector<std::string>> command_argv(const std::string& declaration) {
        auto manifest = kaixa::parse_manifest_document_string(
            "[package]\n"
            "name = \"tasks\"\n"
            "\n"
            "[command.probe]\n"
                + declaration,
            "commands.toml"
        );
        if (!manifest)
            return std::unexpected(manifest.error());

        return manifest->package->commands.front().run;
    }
}

KAIXA_TEST(command_run_accepts_a_single_line) {
    const auto argv = command_argv("run = \"python tools/quality.py tidy\"\n");
    context.check(argv.has_value(), "a single line parses");
    if (!argv) {
        context.fail(kaixa::format_diagnostic(argv.error()));
        return;
    }

    context.check_equal(argv->size(), std::size_t{3}, "the line splits into three arguments");
    context.check_equal(argv->front(), std::string("python"), "the program leads the argv");
    context.check_equal(argv->back(), std::string("tidy"), "the last argument is retained");
}

KAIXA_TEST(command_run_collapses_repeated_separators) {
    const auto argv = command_argv("run = \"  cmake   -E\tesho  \"\n");
    context.check(argv.has_value(), "extra spacing parses");
    if (!argv)
        return;

    context.check_equal(argv->size(), std::size_t{3}, "runs of separators do not create empty arguments");
    context.check_equal(argv->front(), std::string("cmake"), "leading separators are ignored");
}

KAIXA_TEST(command_run_keeps_quoted_arguments_together) {
    const auto argv = command_argv("run = \"python -c 'print(1, 2)'\"\n");
    context.check(argv.has_value(), "quoted arguments parse");
    if (!argv) {
        context.fail(kaixa::format_diagnostic(argv.error()));
        return;
    }

    context.check_equal(argv->size(), std::size_t{3}, "a quoted run stays one argument");
    context.check_equal(argv->back(), std::string("print(1, 2)"), "quotes are removed from the argument");
}

KAIXA_TEST(command_run_keeps_an_empty_quoted_argument) {
    const auto argv = command_argv("run = \"tool '' next\"\n");
    context.check(argv.has_value(), "an empty quoted argument parses");
    if (!argv)
        return;

    context.check_equal(argv->size(), std::size_t{3}, "the empty argument survives");
    context.check(argv->at(1).empty(), "the empty argument is empty");
}

KAIXA_TEST(command_run_rejects_an_unterminated_quote) {
    const auto argv = command_argv("run = \"python -c 'print(1)\"\n");
    context.check(!argv.has_value(), "an unterminated quote fails");
    if (!argv)
        context.check_contains(kaixa::format_diagnostic(argv.error()), "unterminated quote", "the diagnostic names the problem");
}

KAIXA_TEST(command_run_rejects_a_blank_line) {
    const auto argv = command_argv("run = \"   \"\n");
    context.check(!argv.has_value(), "a blank line fails");
    if (!argv)
        context.check_contains(kaixa::format_diagnostic(argv.error()), "cannot be empty", "the diagnostic reuses the empty-run message");
}

KAIXA_TEST(command_run_still_accepts_an_array) {
    const auto argv = command_argv("run = [\"python\", \"tools/quality.py\", \"tidy\"]\n");
    context.check(argv.has_value(), "the array form still parses");
    if (!argv)
        return;

    context.check_equal(argv->size(), std::size_t{3}, "the array is taken verbatim");
    context.check_equal(argv->at(1), std::string("tools/quality.py"), "array arguments are not split");
}

KAIXA_TEST(command_run_does_not_split_interpolation) {
    const auto argv = command_argv("run = \"tool ${build-dir}\"\n");
    context.check(argv.has_value(), "interpolation parses");
    if (!argv)
        return;

    context.check_equal(argv->size(), std::size_t{2}, "interpolation stays one argument");
    context.check_equal(argv->back(), std::string("${build-dir}"), "interpolation is left for the task layer");
}
