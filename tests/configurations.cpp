#include <test_support.hpp>

#include <kaixa/kaixa.hpp>
#include <kaixa/plugin/bundle.hpp>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using kaixa::testing::TempDirectory;

KAIXA_TEST(configurations_compose_published_local_and_cli_layers) {
    const TempDirectory root("build-configurations");
    root.write(
        "published.toml",
        "[build]\ndefault-configs = [\"dev\"]\n"
        "\n[[config]]\nname = \"dev\"\nprofile = \"debug\"\n"
        "\n[config.cmake]\narguments = [\"-DDEV=ON\"]\n"
    );
    root.write(
        "user.toml",
        "[build]\ndefault-configs = [\"clang\"]\n"
        "\n[[config]]\nname = \"clang\"\n"
        "\n[config.cmake]\n"
        "generator = \"Ninja\"\ncxx-compiler = \"clang++\"\n"
    );

    auto published = kaixa::parse_configuration_file(root.path() / "published.toml");
    auto user = kaixa::parse_configuration_file(root.path() / "user.toml");
    context.check(published.has_value() && user.has_value(), "configuration files parse");
    if (!published || !user)
        return;

    const std::vector<kaixa::ConfigurationSet> layers{*published, *user};
    const std::vector<std::string> requested;
    const std::vector<kaixa::ResolverArgumentOverride> overrides{
        {"cmake", {"-DBUILD_TESTING=ON"}, {}},
        {"cmake", {"--verbose"}, "build"},
        {"lua", {"--trace"}, {}}
    };
    const auto effective = kaixa::resolve_configurations(
        layers,
        requested,
        std::nullopt,
        overrides
    );
    context.check(effective.has_value(), "configuration layers resolve");
    if (!effective)
        return;

    context.check_equal(effective->profile, std::string("debug"), "published profile");
    context.check_equal(
        std::filesystem::path(effective->profile_origin.source).filename().string(),
        std::string("published.toml"),
        "profile origin"
    );
    context.check_equal(effective->selected.size(), std::size_t{2}, "both defaults selected");
    const kaixa::ResolverBuildConfiguration* cmake = effective->find("cmake");
    context.check(cmake != nullptr && cmake->settings.has_value(), "CMake settings exist");
    if (!cmake || !cmake->settings)
        return;

    auto settings_result = kaixa::TableReader::bind(*cmake->settings);
    if (!settings_result) {
        context.fail(kaixa::format_diagnostic(settings_result.error()));
        return;
    }
    kaixa::TableReader settings = std::move(*settings_result);
    const auto generator = settings.optional_string("generator");
    const auto compiler = settings.optional_string("cxx-compiler");
    const kaixa::Value* arguments = settings.take("arguments");
    context.check(generator && *generator == "Ninja", "local generator merged");
    context.check(compiler && *compiler == "clang++", "local compiler merged");
    const kaixa::Value* generator_value = cmake->settings->find("generator");
    context.check(generator_value != nullptr, "merged generator value exists");
    if (generator_value) {
        context.check_equal(
            std::filesystem::path(generator_value->location().source).filename().string(),
            std::string("user.toml"),
            "merged generator origin"
        );
    }
    context.check(arguments && arguments->as_array(), "published resolver arguments preserved");
    context.check_equal(cmake->arguments.size(), std::size_t{1}, "CLI argument is separate");
    context.check_equal(cmake->scoped_arguments.size(), std::size_t{1}, "scoped CLI argument is separate");
    if (!cmake->scoped_arguments.empty()) {
        context.check_equal(
            cmake->scoped_arguments.front().scope,
            std::string("build"),
            "build argument scope"
        );
    }
    const kaixa::ResolverBuildConfiguration* lua = effective->find("lua");
    context.check(lua != nullptr, "second resolver override exists");
    if (lua) {
        context.check_equal(lua->arguments.size(), std::size_t{1}, "Lua has only its argument");
        context.check_equal(lua->arguments.front(), std::string("--trace"), "Lua argument");
    }
}

KAIXA_TEST(cmake_consumes_a_named_configuration) {
    const TempDirectory root("cmake-named-configuration");
    kaixa::Manifest manifest{"configured", "cmake"};
    manifest.configurations.defaults.push_back("clang");
    kaixa::ConfigurationDefinition clang;
    clang.name = "clang";
    clang.resolvers.push_back({
        "cmake",
        kaixa::Value::table({
            {"generator", "Ninja"},
            {"cxx-compiler", "clang++"},
            {"arguments", kaixa::Value::array({"-DBUILD_TESTING=OFF"})}
        })
    });
    manifest.configurations.definitions.push_back(std::move(clang));
    root.write_manifest("Kaixa.toml", manifest);
    root.write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)\n");

    const auto graph = kaixa::load_workspace(root.path());
    if (!graph) {
        context.fail(kaixa::format_diagnostic(graph.error()));
        return;
    }
    const kaixa::PackageNode& package = (*graph)[graph->root()];
    const std::vector<kaixa::ConfigurationSet> layers{package.manifest->configurations};
    const std::vector<std::string> requested;
    const std::vector<kaixa::ResolverArgumentOverride> overrides;
    auto configuration = kaixa::resolve_configurations(
        layers,
        requested,
        std::nullopt,
        overrides
    );
    if (!configuration) {
        context.fail(kaixa::format_diagnostic(configuration.error()));
        return;
    }

    const kaixa::BuildEnvironment environment{
        root.path(),
        root.path() / "out",
        std::move(*configuration)
    };
    const kaixa::ResolverRegistry registry = kaixa::plugin::default_registry();
    const auto plan = kaixa::plan_build(*graph, registry, environment);
    context.check(plan.has_value(), "configured CMake project plans");
    if (!plan || plan->actions().empty())
        return;

    const std::vector<std::string> command = plan->actions().front().argv;
    for (const std::string& expected: {
             std::string("Ninja"),
             std::string("-DCMAKE_CXX_COMPILER=clang++"),
             std::string("-DBUILD_TESTING=OFF")
         }) {
        context.check(
            std::ranges::find(command, expected) != command.end(),
            "configure command contains " + expected
        );
    }
}

KAIXA_TEST(configuration_selection_rejects_an_unknown_name) {
    const std::vector<kaixa::ConfigurationSet> layers(1);
    const std::vector<std::string> requested{"missing"};
    const std::vector<kaixa::ResolverArgumentOverride> overrides;
    const auto configuration = kaixa::resolve_configurations(
        layers,
        requested,
        std::nullopt,
        overrides
    );
    context.check(!configuration.has_value(), "unknown configuration is rejected");
    if (!configuration) {
        context.check_contains(
            kaixa::format_diagnostic(configuration.error()),
            "unknown build configuration `missing`",
            "diagnostic names the configuration"
        );
    }
}
