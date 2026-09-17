#include <kaixa/kaixa.hpp>
#include <test_support.hpp>

#include <string>

KAIXA_TEST(diagnostic_sink_collects_every_unknown_key) {
    kaixa::DiagnosticSink sink;
    const auto manifest = kaixa::parse_manifest_document_string(
        "[package]\n"
        "name = \"probe\"\n"
        "version = \"0.1.0\"\n"
        "resolver = \"cmake\"\n"
        "msvc-runtim = \"static\"\n"
        "\n"
        "[command.probe]\n"
        "run = \"cmake -E true\"\n"
        "inpts = [\"probe.txt\"]\n",
        "probe.toml",
        &sink
    );

    context.check(sink.errors() >= std::size_t{2}, "both tables report their unknown key");
    if (sink.errors() < std::size_t{2}) {
        for (const kaixa::Diagnostic& diagnostic: sink.diagnostics())
            context.fail(kaixa::format_diagnostic(diagnostic));

        return;
    }

    bool package_reported = false;
    bool command_reported = false;
    for (const kaixa::Diagnostic& diagnostic: sink.diagnostics()) {
        if (!diagnostic.location)
            continue;

        package_reported = package_reported || diagnostic.location->config_path == "package.msvc-runtim";
        command_reported = command_reported || diagnostic.location->config_path == "command.probe.inpts";
    }
    context.check(package_reported, "the key under package is located");
    context.check(command_reported, "the key under the command is located");
    context.check(manifest.has_value(), "parsing keeps going so later tables are still validated");
}

KAIXA_TEST(diagnostic_sink_keeps_distinct_positions) {
    kaixa::DiagnosticSink sink;
    const auto manifest = kaixa::parse_manifest_document_string(
        "[package]\n"
        "name = \"probe\"\n"
        "first = 1\n"
        "second = 2\n",
        "probe.toml",
        &sink
    );
    context.check(manifest.has_value(), "the document still parses");
    context.check_equal(sink.errors(), std::size_t{2}, "each unknown key is its own diagnostic");
    if (sink.diagnostics().size() < 2)
        return;

    const kaixa::Diagnostic& first = sink.diagnostics().front();
    const kaixa::Diagnostic& second = sink.diagnostics().back();
    context.check(first.location && second.location, "both diagnostics carry a position");
    if (first.location && second.location)
        context.check(first.location->line != second.location->line, "the positions differ");

    context.check(first.notes.empty(), "a sink does not fold the extras into notes");
}

KAIXA_TEST(unknown_keys_still_fail_without_a_sink) {
    const auto manifest = kaixa::parse_manifest_document_string(
        "[package]\n"
        "name = \"probe\"\n"
        "first = 1\n"
        "second = 2\n",
        "probe.toml"
    );
    context.check(!manifest.has_value(), "without a sink the first unknown key still fails");
    if (!manifest) {
        context.check_contains(kaixa::format_diagnostic(manifest.error()), "unknown key", "the message is unchanged");
        context.check_equal(manifest.error().notes.size(), std::size_t{1}, "the extra key is still a note");
    }
}

KAIXA_TEST(diagnostic_severity_reaches_the_formatters) {
    const kaixa::Diagnostic warning = kaixa::warning_at({"probe.toml", 3, 5, "package.first"}, "prefer another key");
    context.check_contains(kaixa::format_diagnostic(warning), "warning: ", "the human format names the severity");
    context.check_contains(kaixa::format_diagnostic_short(warning), ":3:5: warning: ", "the short format names the severity");

    const kaixa::Diagnostic failure = kaixa::error_at({"probe.toml", 1, 1, {}}, "broken");
    context.check_contains(kaixa::format_diagnostic_short(failure), ":1:1: error: ", "errors keep their severity");
}
