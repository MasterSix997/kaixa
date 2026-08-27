#include "application_internal.hpp"

#include <kaixa/foundation/filesystem.hpp>

#include <algorithm>
#include <iostream>
#include <optional>
#include <utility>

namespace kaixa::cli::detail {
    Result<const PackageProvider*> select_catalog_provider(const ExtensionRegistry& registry, const std::optional<std::string>& requested) {
        if (requested) {
            const PackageProvider* provider = registry.find_provider(*requested);
            if (!provider)
                return std::unexpected(error("package provider `" + *requested + "` is not configured"));

            return provider;
        }

        const PackageProvider* selected = nullptr;
        for (const auto& provider: registry.providers()) {
            if (provider->info().is_default)
                selected = provider.get();
        }
        if (selected)
            return selected;

        if (registry.providers().size() == 1)
            return registry.providers().front().get();

        return std::unexpected(error("no default package provider is configured").add_note("select one with `--provider <name>`"));
    }

    Result<PackageCandidate> select_add_candidate(
        const PackageProvider& provider,
        const std::string& package,
        const std::optional<std::string>& requirement
    ) {
        std::optional<VersionRequirement> parsed_requirement;
        if (requirement) {
            auto parsed = parse_version_requirement(*requirement);
            if (!parsed)
                return std::unexpected(parsed.error());

            parsed_requirement = std::move(*parsed);
        }
        auto candidates = provider.candidates(PackageRequest{package, parsed_requirement});
        if (!candidates)
            return std::unexpected(candidates.error());

        std::optional<std::size_t> selected;
        for (std::size_t index = 0; index < candidates->size(); ++index) {
            const PackageCandidate& candidate = (*candidates)[index];
            if (!candidate.version || (parsed_requirement && !matches(*parsed_requirement, *candidate.version)))
                continue;

            if (!selected || compare_versions(*candidate.version, *(*candidates)[*selected].version) > 0)
                selected = index;
        }
        if (!selected) {
            return std::unexpected(error("provider `" + provider.info().name + "` has no compatible version of `" + package + "`"));
        }
        return std::move((*candidates)[*selected]);
    }

    int run(const SearchCommand& command) {
        WorkspaceOptions options = command.workspace;
        options.lock_mode = LockMode::none;
        auto workspace = open_workspace(options);
        if (!workspace)
            return fail(workspace.error());

        PackageQuery query{command.query, command.resolver, command.capability, command.tag, command.limit};
        std::vector<PackageSummary> results;
        for (const auto& provider: workspace->registry.providers()) {
            if (command.provider && provider->info().name != *command.provider)
                continue;

            auto found = provider->query(query);
            if (!found)
                return fail(found.error());

            results.insert(results.end(), found->begin(), found->end());
        }
        std::ranges::sort(results, [](const PackageSummary& left, const PackageSummary& right) {
            if (left.name != right.name)
                return left.name < right.name;

            if (left.provider != right.provider)
                return left.provider < right.provider;

            return compare_versions(left.version, right.version) > 0;
        });
        if (results.empty()) {
            std::cout << "no packages found\n";
            return 0;
        }
        for (const PackageSummary& package: results) {
            std::cout << package.name << ' ' << package.version.text << " [" << package.provider << ']';
            if (!package.description.empty())
                std::cout << " - " << package.description;

            std::cout << '\n';
        }
        return 0;
    }

    int run(const InfoCommand& command) {
        WorkspaceOptions options = command.workspace;
        options.lock_mode = LockMode::none;
        auto workspace = open_workspace(options);
        if (!workspace)
            return fail(workspace.error());

        PackageQuery query{command.package};
        bool found = false;
        for (const auto& provider: workspace->registry.providers()) {
            if (command.provider && provider->info().name != *command.provider)
                continue;

            auto packages = provider->query(query);
            if (!packages)
                return fail(packages.error());

            for (const PackageSummary& package: *packages) {
                if (package.name != command.package)
                    continue;

                found = true;
                std::cout
                    << package.name
                    << ' '
                    << package.version.text
                    << "\n  provider: "
                    << package.provider
                    << "\n  authority: "
                    << package.authority;
                if (package.resolver)
                    std::cout << "\n  resolver: " << *package.resolver;

                if (!package.description.empty())
                    std::cout << "\n  description: " << package.description;

                if (!package.capabilities.empty()) {
                    std::cout << "\n  capabilities: ";
                    for (std::size_t index = 0; index < package.capabilities.size(); ++index) {
                        if (index != 0)
                            std::cout << ", ";

                        std::cout << package.capabilities[index];
                    }
                }
                if (!package.tags.empty()) {
                    std::cout << "\n  tags: ";
                    for (std::size_t index = 0; index < package.tags.size(); ++index) {
                        if (index != 0)
                            std::cout << ", ";

                        std::cout << package.tags[index];
                    }
                }
                std::cout << '\n';
            }
        }
        if (!found)
            return fail(error("package `" + command.package + "` was not found in the selected catalogs"));

        return 0;
    }

    int run(const AddCommand& command) {
        WorkspaceOptions catalog_options = command.workspace;
        catalog_options.lock_mode = LockMode::none;
        auto workspace = open_workspace(catalog_options);
        if (!workspace)
            return fail(workspace.error());

        auto provider = select_catalog_provider(workspace->registry, command.provider);
        if (!provider)
            return fail(provider.error());

        auto candidate = select_add_candidate(**provider, command.package, command.version);
        if (!candidate)
            return fail(candidate.error());

        auto manifest = selected_manifest(command.workspace);
        if (!manifest)
            return fail(manifest.error());

        const std::string requirement = command.version.value_or("^" + candidate->version->text);
        const std::optional<std::string> from = command.provider || !(*provider)->info().is_default
            ? std::optional<std::string>{(*provider)->info().name}
            : std::nullopt;
        auto edit = add_manifest_dependency(*manifest, command.package, requirement, from);
        if (!edit)
            return fail(edit.error());

        print_edit(*edit);
        if (command.dry_run)
            return 0;

        auto applied = apply_manifest_edit(*edit);
        if (!applied)
            return fail(applied.error());

        auto resolved = open_workspace(command.workspace);
        if (!resolved) {
            auto restored = write_file_atomic(edit->path, edit->before);
            Diagnostic diagnostic = resolved.error();
            diagnostic.notes.emplace_back("manifest edit was rolled back");
            if (!restored)
                diagnostic.notes.push_back("rollback failed: " + format_diagnostic(restored.error()));

            return fail(diagnostic);
        }
        std::cout << "added " << command.package << ' ' << requirement << " from " << (*provider)->info().name << '\n';
        return 0;
    }

    int run(const RemoveCommand& command) {
        auto manifest = selected_manifest(command.workspace);
        if (!manifest)
            return fail(manifest.error());

        auto edit = remove_manifest_dependency(*manifest, command.package);
        if (!edit)
            return fail(edit.error());

        print_edit(*edit);
        if (command.dry_run)
            return 0;

        auto applied = apply_manifest_edit(*edit);
        if (!applied)
            return fail(applied.error());

        auto resolved = open_workspace(command.workspace);
        if (!resolved) {
            auto restored = write_file_atomic(edit->path, edit->before);
            Diagnostic diagnostic = resolved.error();
            diagnostic.notes.emplace_back("manifest edit was rolled back");
            if (!restored)
                diagnostic.notes.push_back("rollback failed: " + format_diagnostic(restored.error()));

            return fail(diagnostic);
        }
        std::cout << "removed " << command.package << '\n';
        return 0;
    }

    int run(const UpdateCommand& command) {
        if (command.workspace.lock_mode == LockMode::locked || command.workspace.lock_mode == LockMode::frozen)
            return fail(error("update cannot be combined with `--locked` or `--frozen`"));

        auto workspace = open_workspace(
            command.workspace,
            command.dependencies,
            command.dependencies.empty(),
            !command.dry_run,
            !command.dry_run
        );
        if (!workspace)
            return fail(workspace.error());

        if (!workspace->lock_changed) {
            std::cout << "all selected dependencies are current\n";
            return 0;
        }
        std::cout << (command.dry_run ? "Kaixa.lock would be updated\n" : "Kaixa.lock updated\n");
        for (const PackageNode& package: workspace->graph.nodes()) {
            if (!package.source || !package.source->version)
                continue;

            if (!command.dependencies.empty() && std::ranges::find(command.dependencies, package.name) == command.dependencies.end()) {
                continue;
            }
            std::cout << "  " << package.name << ' ' << package.source->version->text << '\n';
        }
        return 0;
    }

    int run(const PublishCommand& command) {
        auto manifest = selected_manifest(command.workspace);
        if (!manifest)
            return fail(manifest.error());

        std::filesystem::path registry;
        std::optional<std::string> endpoint;
        if (command.registry.contains("://")) {
            endpoint = command.registry;
        } else {
            registry = command.registry;
            if (registry.is_relative())
                registry = std::filesystem::absolute(command.workspace.path / registry).lexically_normal();
        }

        std::optional<std::filesystem::path> prebuilt = command.prebuilt;
        if (prebuilt && prebuilt->is_relative())
            *prebuilt = std::filesystem::absolute(command.workspace.path / *prebuilt).lexically_normal();

        auto published = publish_package(
            {manifest->parent_path(),
                std::move(registry),
                std::move(endpoint),
                command.token_environment,
                std::move(prebuilt),
                command.dry_run}
        );
        if (!published)
            return fail(published.error());

        std::cout
            << (command.dry_run ? "would publish " : "published ")
            << published->package
            << ' '
            << published->version.text
            << (published->prebuilt ? " prebuilt" : " source")
            << "\n  archive: "
            << published->archive
            << "\n  sha256: "
            << published->integrity
            << '\n';
        return 0;
    }

}
