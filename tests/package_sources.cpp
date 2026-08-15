#include <test_support.hpp>

#include <kaixa/kaixa.hpp>
#include <kaixa/plugin/bundle.hpp>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using kaixa::testing::TempDirectory;

namespace {
    class TestSourceDriver final : public kaixa::SourceDriver {
    public:
        [[nodiscard]] kaixa::SourceDriverInfo info() const override {
            return {"test_source", "opens source trees declared by tests"};
        }

        [[nodiscard]] kaixa::Result<std::optional<kaixa::SourceTree>> locate(
            const kaixa::SourceLocator& source,
            const kaixa::SourceContext& context
        ) const override {
            const kaixa::Value* path = source.options.find("path");
            if (!path || !path->as_string())
                return std::unexpected(kaixa::error("test source requires a string `path`"));

            std::filesystem::path directory = *path->as_string();
            if (directory == "missing")
                return std::optional<kaixa::SourceTree>{};

            if (directory.is_relative())
                directory = context.requester / directory;

            directory = std::filesystem::absolute(directory);
            return std::optional{kaixa::SourceTree{directory, directory.generic_string()}};
        }
    };

    class TestProvider final : public kaixa::PackageProvider {
    public:
        TestProvider(kaixa::ProviderInfo info, std::vector<kaixa::PackageCandidate> candidates)
            : m_info(std::move(info)), m_candidates(std::move(candidates)) {
        }

        [[nodiscard]] kaixa::ProviderInfo info() const override {
            return m_info;
        }

        [[nodiscard]] kaixa::Result<std::vector<kaixa::PackageCandidate>> candidates(const kaixa::PackageRequest& request) const override {
            std::vector<kaixa::PackageCandidate> result;
            for (const kaixa::PackageCandidate& candidate: m_candidates) {
                if (candidate.package == request.package)
                    result.push_back(candidate);
            }
            return result;
        }

    private:
        kaixa::ProviderInfo m_info;
        std::vector<kaixa::PackageCandidate> m_candidates;
    };

    kaixa::SourceLocator test_source(const std::filesystem::path& path) {
        return {
            "test_source",
            kaixa::Value::table({{"path", path.generic_string()}})
        };
    }

    kaixa::PackageCandidate candidate(
        std::string package,
        std::string version,
        const std::filesystem::path& path,
        std::string authority = "tests"
    ) {
        return {
            std::move(package),
            kaixa::Version{std::move(version)},
            std::move(authority),
            test_source(path)
        };
    }

    kaixa::PackageCandidate path_candidate(
        std::string package,
        std::string version,
        const std::filesystem::path& path
    ) {
        return {
            std::move(package),
            kaixa::Version{std::move(version)},
            "tests",
            {
                "path",
                kaixa::Value::table({{"path", path.generic_string()}})
            }
        };
    }
}

KAIXA_TEST(direct_source_opens_a_monorepo_and_resolves_internal_packages) {
    const TempDirectory root("direct-package-source");
    const std::filesystem::path source = root.path() / "engine-source";
    root.write(
        "Kaixa.toml",
        "[package]\n"
        "name = \"app\"\n"
        "resolver = \"cmake\"\n"
        "\n"
        "[dependencies]\n"
        "engine = { test_source = { path = \"" + source.generic_string() + "\" } }\n"
    );
    root.write(
        "engine-source/Kaixa.toml",
        "[package-set]\n"
        "members = [\"packages/*\"]\n"
    );
    root.write(
        "engine-source/packages/engine/Kaixa.toml",
        "[package]\n"
        "name = \"engine\"\n"
        "version = \"1.2.0\"\n"
        "resolver = \"cmake\"\n"
        "\n"
        "[dependencies]\n"
        "math = \"1\"\n"
    );
    root.write(
        "engine-source/packages/math/Kaixa.toml",
        "[package]\n"
        "name = \"math\"\n"
        "version = \"1.0.0\"\n"
        "resolver = \"cmake\"\n"
    );

    kaixa::ExtensionRegistry extensions;
    extensions.add(std::make_unique<TestSourceDriver>());
    const auto resolved = kaixa::resolve_workspace(
        root.path(),
        kaixa::ResolutionOptions{{}, &extensions, {}}
    );
    context.check(resolved.has_value(), "direct source resolves");
    if (!resolved) {
        context.fail(kaixa::format_diagnostic(resolved.error()));
        return;
    }

    context.check_equal(resolved->graph.size(), std::size_t{3}, "root, remote package and internal dependency");
    const std::optional<kaixa::PackageId> engine = resolved->graph.find_by_name("engine");
    const std::optional<kaixa::PackageId> math = resolved->graph.find_by_name("math");
    context.check(engine.has_value() && math.has_value(), "monorepo packages enter the graph");
    if (engine) {
        const std::optional<kaixa::PackageSource>& origin = resolved->graph[*engine].source;
        context.check(origin.has_value(), "direct package keeps source metadata");
        if (origin) {
            context.check(!origin->provider.has_value(), "direct source has no provider instance");
            context.check_equal(origin->authority, std::string("direct"), "direct authority");
            context.check_equal(origin->locator.driver, std::string("test_source"), "source driver");
        }
    }
}

KAIXA_TEST(path_dependency_opens_a_local_package_set_directly) {
    const TempDirectory root("path-package-source");
    root.write(
        "Kaixa.toml",
        "[package]\n"
        "name = \"app\"\n"
        "resolver = \"cmake\"\n"
        "\n"
        "[dependencies]\n"
        "engine = { path = \"engine-source\" }\n"
    );
    root.write(
        "engine-source/Kaixa.toml",
        "[package-set]\n"
        "members = [\"packages/*\"]\n"
    );
    root.write(
        "engine-source/packages/engine/Kaixa.toml",
        "[package]\n"
        "name = \"engine\"\n"
        "version = \"1.0.0\"\n"
        "resolver = \"cmake\"\n"
    );

    const auto resolved = kaixa::resolve_workspace(root.path());
    context.check(resolved.has_value(), "direct path resolves a package set");
    if (!resolved) {
        context.fail(kaixa::format_diagnostic(resolved.error()));
        return;
    }

    const std::optional<kaixa::PackageId> engine = resolved->graph.find_by_name("engine");
    context.check(engine.has_value(), "directory package enters the graph");
    if (!engine)
        return;

    const std::optional<kaixa::PackageSource>& source = resolved->graph[*engine].source;
    context.check(source.has_value(), "path source metadata is retained");
    if (!source)
        return;

    context.check_equal(
        source->locator.driver,
        std::string("path"),
        "path remains observable as a path source"
    );
}

KAIXA_TEST(default_provider_selects_the_highest_compatible_candidate) {
    const TempDirectory root("default-package-provider");
    root.write(
        "Kaixa.toml",
        "[package]\n"
        "name = \"app\"\n"
        "resolver = \"cmake\"\n"
        "\n"
        "[dependencies]\n"
        "engine = \"^1.0\"\n"
    );
    root.write(
        "engine-1.0/Kaixa.toml",
        "[package]\n"
        "name = \"engine\"\n"
        "version = \"1.0.0\"\n"
        "resolver = \"cmake\"\n"
    );
    root.write(
        "engine-1.4/Kaixa.toml",
        "[package]\n"
        "name = \"engine\"\n"
        "version = \"1.4.0\"\n"
        "resolver = \"cmake\"\n"
    );
    root.write(
        "engine-2.0/Kaixa.toml",
        "[package]\n"
        "name = \"engine\"\n"
        "version = \"2.0.0\"\n"
        "resolver = \"cmake\"\n"
    );

    std::vector<kaixa::PackageCandidate> candidates;
    candidates.push_back(path_candidate("engine", "1.0.0", root.path() / "engine-1.0"));
    candidates.push_back(path_candidate("engine", "2.0.0", root.path() / "engine-2.0"));
    candidates.push_back(path_candidate("engine", "1.4.0", root.path() / "engine-1.4"));

    kaixa::ExtensionRegistry extensions = kaixa::plugin::default_registry();
    extensions.add(std::make_unique<TestProvider>(
        kaixa::ProviderInfo{"official", "test_provider", true},
        std::move(candidates)
    ));
    const auto resolved = kaixa::resolve_workspace(
        root.path(),
        kaixa::ResolutionOptions{{}, &extensions, {}}
    );
    context.check(resolved.has_value(), "default provider resolves");
    if (!resolved) {
        context.fail(kaixa::format_diagnostic(resolved.error()));
        return;
    }

    const std::optional<kaixa::PackageId> engine = resolved->graph.find_by_name("engine");
    context.check(engine.has_value(), "provider package enters the graph");
    if (!engine)
        return;

    const kaixa::PackageNode& package = resolved->graph[*engine];
    context.check_equal(package.manifest->version->text, std::string("1.4.0"), "highest compatible version");
    context.check(package.source.has_value(), "provider source metadata is retained");
    if (package.source) {
        context.check_equal(package.source->provider.value_or(""), std::string("official"), "provider instance");
        context.check_equal(package.source->authority, std::string("tests"), "resolved authority");
    }
}

KAIXA_TEST(explicit_provider_overrides_the_default_route) {
    const TempDirectory root("explicit-package-provider");
    root.write(
        "Kaixa.toml",
        "[package]\n"
        "name = \"app\"\n"
        "resolver = \"cmake\"\n"
        "\n"
        "[dependencies]\n"
        "engine = { version = \"1\", from = \"company\" }\n"
    );
    root.write(
        "official/Kaixa.toml",
        "[package]\n"
        "name = \"engine\"\n"
        "version = \"1.1.0\"\n"
        "resolver = \"cmake\"\n"
    );
    root.write(
        "company/Kaixa.toml",
        "[package]\n"
        "name = \"engine\"\n"
        "version = \"1.2.0\"\n"
        "resolver = \"cmake\"\n"
    );

    kaixa::ExtensionRegistry extensions;
    extensions.add(std::make_unique<TestSourceDriver>());
    extensions.add(std::make_unique<TestProvider>(
        kaixa::ProviderInfo{"official", "test_provider", true},
        std::vector{candidate("engine", "1.1.0", root.path() / "official", "official")}
    ));
    extensions.add(std::make_unique<TestProvider>(
        kaixa::ProviderInfo{"company", "test_provider", false},
        std::vector{candidate("engine", "1.2.0", root.path() / "company", "company")}
    ));

    const auto resolved = kaixa::resolve_workspace(
        root.path(),
        kaixa::ResolutionOptions{{}, &extensions, {}}
    );
    context.check(resolved.has_value(), "explicit provider resolves");
    if (!resolved) {
        context.fail(kaixa::format_diagnostic(resolved.error()));
        return;
    }

    const std::optional<kaixa::PackageId> engine_id = resolved->graph.find_by_name("engine");
    context.check(engine_id.has_value(), "provider package enters the graph");
    if (!engine_id)
        return;

    const kaixa::PackageNode& engine = resolved->graph[*engine_id];
    context.check(engine.source.has_value(), "provider source metadata is retained");
    if (!engine.source)
        return;

    context.check_equal(engine.source->provider.value_or(""), std::string("company"), "explicit provider wins");
    context.check_equal(engine.source->authority, std::string("company"), "explicit authority");
}
