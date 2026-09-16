#include <test_support.hpp>

#include <command_line.hpp>

#include <array>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {
    std::string usage_text() {
        std::ostringstream out;
        kaixa::cli::print_usage(out);
        return std::move(out).str();
    }

    // A documented name must be one the dispatch knows. Commands with required arguments still
    // fail to parse on their own, so the property is "not rejected as unknown".
    bool dispatches(const std::string_view name) {
        const std::array<std::string_view, 1> arguments{name};
        const auto parsed = kaixa::cli::parse_command_line(arguments);
        return parsed.has_value() || !parsed.error().message.starts_with("unknown command");
    }
}

KAIXA_TEST(usage_lists_every_command_the_dispatch_accepts) {
    const std::string usage = usage_text();
    // Every command a user can type must appear in the usage the same run prints.
    for (
        const std::string_view name: {"inspect",
            "check",
            "generate",
            "build",
            "install",
            "test",
            "bench",
            "run",
            "task",
            "workflow",
            "clean",
            "search",
            "info",
            "add",
            "remove",
            "update",
            "publish",
            "config"}
    ) {
        const std::string entry = "kaixa " + std::string(name);
        context.check_contains(usage, entry, std::string("usage documents `") + std::string(name) + "`");
    }
}

KAIXA_TEST(usage_documents_no_command_the_dispatch_rejects) {
    const std::string usage = usage_text();
    std::vector<std::string> documented;
    std::size_t position = 0;
    const std::string marker = "  kaixa ";
    while ((position = usage.find(marker, position)) != std::string::npos) {
        position += marker.size();
        const std::size_t end = usage.find_first_of(" \n", position);
        std::string name = usage.substr(position, end == std::string::npos ? end : end - position);
        if (!name.empty() && name != "--version")
            documented.push_back(std::move(name));
    }

    context.check(!documented.empty(), "usage names at least one command");
    for (const std::string& name: documented)
        context.check(dispatches(name), std::string("`") + name + "` dispatches");
}

KAIXA_TEST(usage_documents_package_selection_and_path_semantics) {
    const std::string usage = usage_text();
    context.check_contains(usage, "--package name", "usage documents explicit package selection");
    context.check_contains(usage, "--package-set", "usage documents package-set selection");
    context.check_contains(usage, "--exclude-package name", "usage documents package exclusions");
    context.check_contains(usage, "--dependency-tests package-set|all", "usage documents dependency test selection");
    context.check_contains(usage, "--path selects the manifest context", "usage distinguishes path from package selection");
}

KAIXA_TEST(an_unknown_command_is_rejected_with_usage) {
    const std::array<std::string_view, 1> arguments{"frobnicate"};
    const auto parsed = kaixa::cli::parse_command_line(arguments);
    context.check(!parsed.has_value(), "an unknown command is rejected");
    if (!parsed) {
        context.check_contains(parsed.error().message, "unknown command `frobnicate`", "the diagnostic names the command");
        context.check(parsed.error().show_usage, "the rejection asks for usage");
    }
}
