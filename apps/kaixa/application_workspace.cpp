#include "application_internal.hpp"

#include <kaixa/foundation/filesystem.hpp>

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace kaixa::cli::detail {
    void print_source_progress(const std::string_view message) {
        std::cout << "source: " << message << '\n';
        std::cout.flush();
    }

    std::optional<std::filesystem::path> user_configuration_path() {
#ifdef _WIN32
        const std::optional<std::string> base = environment_variable("APPDATA");
        if (base)
            return std::filesystem::path(*base) / "Kaixa" / "config.toml";

#else
        const std::optional<std::string> xdg = environment_variable("XDG_CONFIG_HOME");
        if (xdg)
            return std::filesystem::path(*xdg) / "kaixa" / "config.toml";

        const std::optional<std::string> home = environment_variable("HOME");
        if (home)
            return std::filesystem::path(*home) / ".config" / "kaixa" / "config.toml";

#endif
        return std::nullopt;
    }

    Result<void> append_configuration_file(
        std::vector<ConfigurationSet>& layers,
        std::vector<ConfigurationSource>& sources,
        std::vector<ProviderLayer>& provider_layers,
        std::vector<AutomationDocument>& automation_layers,
        const bool allow_automation,
        std::string name,
        const std::filesystem::path& path
    ) {
        std::error_code failure;
        const bool exists = std::filesystem::exists(path, failure);
        if (failure == std::make_error_code(std::errc::no_such_file_or_directory))
            return {};

        if (failure) {
            return std::unexpected(error("cannot inspect configuration file `" + path.string() + "`: " + failure.message()));
        }
        if (!exists)
            return {};

        if (!std::filesystem::is_regular_file(path, failure) || failure) {
            return std::unexpected(error("configuration path is not a regular file: " + path.string()));
        }

        auto document = parse_configuration_document_file(path);
        if (!document)
            return std::unexpected(document.error());

        sources.push_back({std::move(name), document->configurations});
        layers.push_back(std::move(document->configurations));
        if (!document->providers.empty()) {
            provider_layers.push_back({std::move(document->providers), ProviderContext{path.parent_path()}});
        }
        if (!document->automation.commands.empty() || !document->automation.workflows.empty()) {
            if (!allow_automation) {
                const SourceLocation& location = !document->automation.commands.empty() ? document->automation.commands.front().location
                                                                                        : document->automation.workflows.front().location;
                return std::unexpected(
                    error_at(location, "local commands and workflows are supported only in workspace `Kaixa.user.toml`")
                );
            }
            automation_layers.push_back(std::move(document->automation));
        }

        return {};
    }

    bool resolver_is_active(const Graph& graph, const std::string_view resolver) {
        return std::ranges::any_of(graph.nodes(), [&](const PackageNode& package) {
            return package.kind == PackageKind::managed && package.resolver == resolver;
        });
    }

    Result<Workspace> open_workspace(
        const WorkspaceOptions& options,
        const std::span<const std::string> unlocked_packages,
        const bool unlock_all,
        const bool write_lock,
        const bool refresh_sources
    ) {
        auto manifest = find_manifest(options.path);
        if (!manifest)
            return std::unexpected(manifest.error());

        const std::filesystem::path directory = manifest->parent_path();
        std::vector<ConfigurationSet> external_layers;
        std::vector<ConfigurationSource> external_sources;
        std::vector<ProviderLayer> provider_layers;
        std::vector<AutomationDocument> automation_layers;
        if (const auto user = user_configuration_path()) {
            auto loaded = append_configuration_file(
                external_layers,
                external_sources,
                provider_layers,
                automation_layers,
                false,
                "user",
                *user
            );
            if (!loaded)
                return std::unexpected(loaded.error());
        }

        auto local = append_configuration_file(
            external_layers,
            external_sources,
            provider_layers,
            automation_layers,
            true,
            "local",
            directory / "Kaixa.user.toml"
        );
        if (!local)
            return std::unexpected(local.error());

        auto manifest_document = parse_manifest_document_file(*manifest);
        if (!manifest_document)
            return std::unexpected(manifest_document.error());

        std::vector<ConfigurationSet> layers;
        std::vector<ConfigurationSource> sources;
        sources.push_back({"manifest", manifest_document->configurations});
        layers.push_back(manifest_document->configurations);
        layers.insert(layers.end(), external_layers.begin(), external_layers.end());
        sources.insert(sources.end(), external_sources.begin(), external_sources.end());

        auto configuration = resolve_configurations(
            layers,
            options.configurations,
            options.profile,
            options.resolver_arguments,
            options.use_default_configurations
        );
        if (!configuration)
            return std::unexpected(configuration.error());

        const ResolverBuildConfiguration* features = configuration->find("features");

        ExtensionRegistry registry = plugin::default_registry();
        ResolutionOptions resolution_options;
        resolution_options.packages = options.packages;
        resolution_options.extensions = &registry;
        resolution_options.provider_layers = provider_layers;
        resolution_options.feature_settings = features && features->settings ? &*features->settings : nullptr;
        resolution_options.policy_context = {configuration->profile, host_target_os()};
        resolution_options.lock_mode = options.lock_mode;
        resolution_options.unlocked_packages = unlocked_packages;
        resolution_options.unlock_all = unlock_all;
        resolution_options.write_lock = write_lock;
        resolution_options.refresh_sources = refresh_sources;
        resolution_options.source_progress = print_source_progress;
        resolution_options.load_model = false;
        auto resolved = resolve_workspace(options.path, resolution_options);
        if (!resolved)
            return std::unexpected(resolved.error());

        auto automation = apply_automation_layers(resolved->graph, std::move(automation_layers));
        if (!automation)
            return std::unexpected(automation.error());

        for (const ResolverArgumentOverride& override: options.resolver_arguments) {
            if (!resolver_is_active(resolved->graph, override.resolver)) {
                return std::unexpected(error("resolver `" + override.resolver + "` does not participate in this build"));
            }
        }

        return Workspace{std::move(resolved->graph),
            resolved->model.summary,
            std::move(resolved->instances),
            BuildEnvironment{directory, directory / ".kaixa", std::move(*configuration)},
            std::move(registry),
            std::move(sources),
            resolved->lock_changed};
    }

    Result<std::filesystem::path> selected_manifest(const WorkspaceOptions& options) {
        auto manifest = find_manifest(options.path);
        if (!manifest)
            return std::unexpected(manifest.error());

        auto document = parse_manifest_document_file(*manifest);
        if (!document)
            return std::unexpected(document.error());

        if (options.packages.size() > 1)
            return std::unexpected(error("package editing accepts at most one `--package` selection"));

        if (options.packages.empty()) {
            if (document->package)
                return *manifest;

            return std::unexpected(error("package editing requires `--package <name>` for a package set"));
        }
        const std::string& name = options.packages.front();
        if (document->package && document->package->name == name)
            return *manifest;

        auto packages = PackageIndex::discover(*manifest, *document);
        if (!packages)
            return std::unexpected(packages.error());

        const auto candidate = std::ranges::find(packages->candidates(), name, &LocalPackageCandidate::name);
        if (candidate == packages->candidates().end())
            return std::unexpected(error("package `" + name + "` is not available for editing"));

        return candidate->manifest;
    }

    void print_edit(const ManifestEdit& edit) {
        std::cout << "--- " << edit.path.string() << '\n' << "+++ " << edit.path.string() << '\n';
        const auto lines = [](const std::string& contents) {
            std::vector<std::string_view> result;
            std::size_t begin = 0;
            while (begin < contents.size()) {
                const std::size_t end = contents.find('\n', begin);
                result.emplace_back(contents.data() + begin, (end == std::string::npos ? contents.size() : end) - begin);
                if (end == std::string::npos)
                    break;

                begin = end + 1;
            }
            return result;
        };
        const std::vector<std::string_view> before = lines(edit.before);
        const std::vector<std::string_view> after = lines(edit.after);
        std::size_t prefix = 0;
        while (prefix < before.size() && prefix < after.size() && before[prefix] == after[prefix])
            ++prefix;

        std::size_t suffix = 0;
        while (
            suffix + prefix < before.size()
            && suffix + prefix < after.size()
            && before[before.size() - suffix - 1] == after[after.size() - suffix - 1]
        ) {
            ++suffix;
        }
        for (std::size_t index = prefix; index < before.size() - suffix; ++index)
            std::cout << '-' << before[index] << '\n';

        for (std::size_t index = prefix; index < after.size() - suffix; ++index)
            std::cout << '+' << after[index] << '\n';
    }

}
