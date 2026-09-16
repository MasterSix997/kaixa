#include <kaixa/workspace/loader.hpp>

#include "dependency_routing.hpp"
#include "feature_activation.hpp"
#include "managed_package.hpp"
#include "package_routes.hpp"
#include "resolution_lock_coordinator.hpp"
#include "source_materialization.hpp"

#include <kaixa/source/cache.hpp>

#include <kaixa/model/manifest.hpp>
#include <kaixa/test/adapter.hpp>
#include <kaixa/workspace/package_index.hpp>

#include <algorithm>
#include <map>
#include <utility>

namespace kaixa {
    namespace {
        class WorkspaceLoader {
        public:
            WorkspaceLoader(
                const ResolutionOptions& options,
                std::filesystem::path source_cache,
                const ManifestDocument* manifest_document = nullptr
            )
                : m_extensions(options.extensions)
                , m_source_cache(std::move(source_cache))
                , m_provider_layers(options.provider_layers)
                , m_feature_settings(options.feature_settings)
                , m_policy_context(options.policy_context)
                , m_locks(options.lock_mode, options.lockfile, options.unlocked_packages, options.unlock_all, options.write_lock)
                , m_refresh_sources(options.refresh_sources)
                , m_source_progress(options.source_progress)
                , m_load_model(options.load_model)
                , m_excluded_packages(options.excluded_packages)
                , m_select_package_set(options.package_set)
                , m_manifest_document(manifest_document) {}

            Result<PackageResolution> load(
                const std::filesystem::path& manifest_path,
                const std::span<const std::string> selected_packages
            ) {
                auto workspace = open_workspace(manifest_path);
                if (!workspace)
                    return std::unexpected(workspace.error());

                m_model = std::move(workspace->tree);

                auto lock = m_locks.read();
                if (!lock)
                    return std::unexpected(lock.error());

                auto providers = configure_context_providers();
                if (!providers)
                    return std::unexpected(providers.error());

                auto selection = selected_package_names(workspace->document, selected_packages);
                if (!selection)
                    return std::unexpected(selection.error());

                auto roots = load_selected_packages(workspace->manifest, workspace->document, *selection);

                if (!roots)
                    return std::unexpected(roots.error());

                auto configured_features = activate_configured_features();
                if (!configured_features)
                    return std::unexpected(configured_features.error());

                for (const PackageId root: *roots)
                    m_graph.add_root(root);

                auto order = m_graph.build_order();
                if (!order)
                    return std::unexpected(order.error());

                auto instances = configure_package_instances(m_graph, m_policy_context, policy_schema());
                if (!instances)
                    return std::unexpected(instances.error());

                auto lock_changed = m_locks.update(m_graph, *instances, m_policy_context, m_context_directory);
                if (!lock_changed)
                    return std::unexpected(lock_changed.error());

                std::vector<std::string> root_names;
                root_names.reserve(m_graph.roots().size());
                for (const PackageId root: m_graph.roots())
                    root_names.push_back(m_graph[root].name);

                std::ranges::sort(root_names);
                ResolutionContext context{m_context_manifest,
                    m_context_directory,
                    m_locks.lockfile(),
                    std::move(root_names),
                    m_policy_context,
                    m_locks.mode()};

                return PackageResolution{std::move(m_graph),
                    std::move(m_packages),
                    std::move(workspace->document.configurations),
                    std::move(workspace->manifest),
                    std::move(context),
                    *lock_changed,
                    std::move(*m_model),
                    std::move(*instances)};
            }

        private:
            struct OpenedWorkspace {
                std::filesystem::path manifest;
                ManifestDocument document;
                ManifestTree tree;
            };

            Result<OpenedWorkspace> open_workspace(const std::filesystem::path& manifest_path) {
                std::error_code failure;
                const std::filesystem::path selected = std::filesystem::canonical(manifest_path, failure);
                if (failure)
                    return std::unexpected(error("cannot canonicalize manifest `" + manifest_path.string() + "`: " + failure.message()));

                Result<ManifestDocument> document = m_manifest_document ? Result<ManifestDocument>{*m_manifest_document}
                                                                        : parse_manifest_document_file(selected);
                if (!document)
                    return std::unexpected(document.error());

                ManifestTree tree;
                if (m_load_model) {
                    auto loaded_tree = load_manifest_tree(selected.parent_path());
                    if (!loaded_tree)
                        return std::unexpected(loaded_tree.error());

                    tree = std::move(*loaded_tree);
                }

                auto packages = m_load_model ? PackageIndex::discover(selected, *document, tree.documents)
                                             : PackageIndex::discover(selected, *document);
                if (!packages)
                    return std::unexpected(packages.error());

                m_packages = std::move(*packages);
                m_context_manifest = m_packages.context_manifests().empty() ? selected : m_packages.context_manifests().front();
                m_context_directory = m_context_manifest.parent_path();
                m_locks.adopt_default_lockfile(m_context_directory);

                return OpenedWorkspace{selected, std::move(*document), std::move(tree)};
            }

            Result<void> activate_configured_features() {
                if (!m_feature_settings)
                    return {};

                const std::vector<TableEntry>* packages = m_feature_settings->as_table();
                if (!packages) {
                    return std::unexpected(
                        error_at(m_feature_settings->location(), "feature configuration must be a package-to-features table")
                    );
                }

                for (const TableEntry& package: *packages) {
                    const auto id = m_graph.find_by_name(package.key);
                    if (!id)
                        continue;

                    const std::vector<Value>* values = package.value.as_array();
                    if (!values) {
                        return std::unexpected(error_at(package.value.location(), "configured package features must be an array"));
                    }
                    std::vector<std::string> features;
                    features.reserve(values->size());
                    for (const Value& value: *values) {
                        const std::string* feature = value.as_string();
                        if (!feature) {
                            return std::unexpected(error_at(value.location(), "configured package features must be strings"));
                        }
                        features.push_back(*feature);
                    }
                    auto activated = activate_features(*id, features, package.value.location());
                    if (!activated)
                        return std::unexpected(activated.error());
                }
                return {};
            }

            Result<void> configure_context_providers() {
                std::vector<ProviderLayer> layers;
                for (const std::filesystem::path& manifest: m_packages.context_manifests()) {
                    auto document = m_packages.load_document(manifest);
                    if (!document)
                        return std::unexpected(document.error());

                    const ManifestDocument& context = **document;
                    for (const auto& [package, provider]: context.routing)
                        m_routing[package] = provider;

                    if (!context.providers.empty()) {
                        layers.push_back({context.providers, ProviderContext{manifest.parent_path(), m_source_cache, m_locks.offline()}});
                    }
                }
                layers.insert(layers.end(), m_provider_layers.begin(), m_provider_layers.end());
                for (ProviderLayer& layer: layers) {
                    if (layer.context.cache.empty())
                        layer.context.cache = m_source_cache;

                    layer.context.offline = layer.context.offline || m_locks.offline();
                }
                if (layers.empty())
                    return {};

                if (!m_extensions)
                    return std::unexpected(error("provider configuration requires an extension registry"));

                return configure_providers(*m_extensions, layers);
            }

            Result<std::vector<std::string>> selected_package_names(
                const ManifestDocument& document,
                const std::span<const std::string> explicit_packages
            ) {
                for (std::size_t index = 0; index < explicit_packages.size(); ++index) {
                    for (std::size_t previous = 0; previous < index; ++previous) {
                        if (explicit_packages[previous] == explicit_packages[index]) {
                            return std::unexpected(error("package `" + explicit_packages[index] + "` was selected more than once"));
                        }
                    }
                }

                std::vector<std::string> selected;
                if (m_select_package_set) {
                    auto context = m_packages.load_document(m_context_manifest);
                    if (!context)
                        return std::unexpected(context.error());

                    if (!(**context).package_set) {
                        return std::unexpected(error("current manifest context does not declare a package set"));
                    }
                    selected.reserve(m_packages.candidates().size() + explicit_packages.size());
                    for (const LocalPackageCandidate& candidate: m_packages.candidates())
                        selected.push_back(candidate.name);
                }

                if (!explicit_packages.empty()) {
                    if (!m_select_package_set)
                        selected.reserve(explicit_packages.size());

                    for (const std::string& name: explicit_packages) {
                        if (std::ranges::find(selected, name) == selected.end())
                            selected.push_back(name);
                    }
                } else if (!m_select_package_set) {
                    if (document.package) {
                        selected.push_back(document.package->name);
                    } else if (!document.package_set) {
                        return std::unexpected(error("manifest does not declare a package"));
                    } else if (document.package_set->defaults.empty()) {
                        return std::unexpected(
                            error_at(document.package_set->location, "package set requires `default` or an explicit package selection")
                        );
                    } else {
                        selected = document.package_set->defaults;
                    }
                }

                for (std::size_t index = 0; index < m_excluded_packages.size(); ++index) {
                    const std::string& name = m_excluded_packages[index];
                    for (std::size_t previous = 0; previous < index; ++previous) {
                        if (m_excluded_packages[previous] == name) {
                            return std::unexpected(error("package `" + name + "` was excluded more than once"));
                        }
                    }

                    const auto excluded = std::ranges::find(selected, name);
                    if (excluded == selected.end())
                        return std::unexpected(error("package `" + name + "` is not selected and cannot be excluded"));

                    selected.erase(excluded);
                }
                if (selected.empty())
                    return std::unexpected(error("package selection is empty after exclusions"));

                return selected;
            }

            Result<std::vector<PackageId>> load_selected_packages(
                const std::filesystem::path& manifest_path,
                const ManifestDocument& document,
                const std::span<const std::string> selected_packages
            ) {
                std::vector<PackageId> roots;
                roots.reserve(selected_packages.size());
                for (std::size_t index = 0; index < selected_packages.size(); ++index) {
                    const std::string& name = selected_packages[index];
                    for (std::size_t previous = 0; previous < index; ++previous) {
                        if (selected_packages[previous] == name) {
                            return std::unexpected(error("package `" + name + "` was selected more than once"));
                        }
                    }

                    const LocalPackageCandidate* candidate = nullptr;
                    if (document.package_set)
                        candidate = m_packages.find_in_set(manifest_path, name);

                    if (!candidate)
                        candidate = m_packages.find_for(manifest_path, name);

                    if (!candidate && document.package && document.package->name == name) {
                        auto root = load_managed(manifest_path, name, document.package->location);
                        if (!root)
                            return std::unexpected(root.error());

                        roots.push_back(*root);
                        continue;
                    }
                    if (!candidate) {
                        Diagnostic diagnostic = error("package `" + name + "` is not available");
                        std::string available;
                        for (const LocalPackageCandidate& package: m_packages.candidates()) {
                            if (!available.empty())
                                available += ", ";

                            available += package.name;
                        }
                        if (document.package && m_packages.candidates().empty())
                            available = document.package->name;

                        if (!available.empty()) {
                            diagnostic.notes.push_back("available packages: " + available);
                        }

                        return std::unexpected(std::move(diagnostic));
                    }

                    auto root = load_managed(candidate->manifest, name, candidate->location);
                    if (!root)
                        return std::unexpected(root.error());

                    roots.push_back(*root);
                }
                return roots;
            }

            Result<PackageId> load_managed(
                const std::filesystem::path& manifest_path,
                const std::optional<std::string_view> expected_name,
                const SourceLocation& declaration
            ) {
                auto directory_result = workspace_detail::canonical_directory(manifest_path.parent_path(), declaration);
                if (!directory_result)
                    return std::unexpected(directory_result.error());

                const std::filesystem::path directory = *directory_result;

                const std::optional<PackageId> existing = expected_name ? m_graph.find_by_name(*expected_name)
                                                                        : m_graph.find_by_directory(directory);
                if (existing && m_graph[*existing].directory == directory)
                    return *existing;

                const std::filesystem::path package_manifest = directory / "Kaixa.toml";
                const ManifestDocument* parsed_document = nullptr;
                if (m_model) {
                    const auto document = std::ranges::find_if(m_model->documents, [&](const KaixaDocument& candidate) {
                        const auto* package = std::get_if<ManifestDocument>(&candidate);
                        return package && package->source == package_manifest;
                    });
                    if (document != m_model->documents.end())
                        parsed_document = std::get_if<ManifestDocument>(&*document);
                }
                if (!parsed_document)
                    parsed_document = m_packages.document(package_manifest);

                auto prepared = workspace_detail::prepare_managed_package(
                    m_packages,
                    directory,
                    expected_name,
                    declaration,
                    parsed_document
                );
                if (!prepared)
                    return std::unexpected(prepared.error());

                const std::filesystem::path canonical_manifest = std::move(prepared->manifest_path);
                Manifest manifest = std::move(prepared->manifest);
                std::vector<PackageTarget> package_targets = std::move(prepared->targets);

                if (const auto same_name = m_graph.find_by_name(manifest.name)) {
                    return std::unexpected(error_at(
                        manifest.location,
                        "package `" + manifest.name + "` is also provided by `" + m_graph[*same_name].directory.string() + "`"
                    ));
                }

                PackageNode node;
                node.name = manifest.name;
                node.directory = directory;
                node.resolver = manifest.resolver;
                node.semantics = ManagedPackage{std::move(manifest)};
                const PackageId id = m_graph.add(std::move(node));
                m_graph[id].policy_layers = m_packages.policies_for(canonical_manifest);

                const std::vector<DependencyBinding> dependencies = m_graph[id].manifest()->dependencies;
                for (const DependencyBinding& dependency: dependencies) {
                    if (dependency.request.optional)
                        continue;

                    auto target = load_dependency(directory, canonical_manifest, dependency);
                    if (!target)
                        return std::unexpected(target.error());

                    m_graph[id].dependencies.push_back(*target);
                }

                auto resolved_targets = resolve_target_dependencies(id, package_targets, canonical_manifest);
                if (!resolved_targets)
                    return std::unexpected(resolved_targets.error());

                m_graph[id].targets = std::move(package_targets);

                const std::vector<std::string> defaults = m_graph[id].manifest()->default_features;
                auto activated = activate_features(id, defaults, m_graph[id].manifest()->location);
                if (!activated)
                    return std::unexpected(activated.error());

                return id;
            }

            Result<void> resolve_target_dependencies(
                const PackageId package,
                const std::span<PackageTarget> package_targets,
                const std::filesystem::path& requester_manifest
            ) {
                for (PackageTarget& package_target: package_targets) {
                    PackageTargetDependencies resolved;
                    resolved.target = *package_target.name;
                    resolved.kind = package_target.kind;
                    std::vector<DependencyBinding> dependencies = package_target.dependencies;
                    const std::string_view framework = package_target.framework
                        ? std::string_view(*package_target.framework)
                        : default_test_adapter(package_target.kind, package_target.discover);
                    if (m_extensions) {
                        auto adapter = test_adapter(*m_extensions, framework, package_target.kind, package_target.location);
                        if (!adapter)
                            return std::unexpected(adapter.error());

                        package_target.adapter = *adapter;
                        if (!adapter->dependency.empty() && std::ranges::none_of(dependencies, [&](const DependencyBinding& dependency) {
                                return dependency.request.package == adapter->dependency;
                            })) {
                            DependencyBinding dependency;
                            dependency.request.package = adapter->dependency;
                            dependency.location = package_target.location;
                            dependencies.push_back(std::move(dependency));
                        }
                    }
                    for (const DependencyBinding& dependency: dependencies) {
                        if (dependency.request.optional)
                            continue;

                        auto target = load_dependency(package_target.source.parent_path(), requester_manifest, dependency);
                        if (!target)
                            return std::unexpected(target.error());

                        if (*target != package && std::ranges::find(resolved.packages, *target) == resolved.packages.end())
                            resolved.packages.push_back(*target);
                    }
                    if (!resolved.packages.empty())
                        m_graph[package].target_dependencies.push_back(std::move(resolved));
                }
                return {};
            }

            [[nodiscard]] workspace_detail::SourceMaterializationContext materialization_context() const {
                return {m_extensions,
                    m_source_cache,
                    m_locks.mode(),
                    m_locks.lock(),
                    m_locks.unlocked_packages(),
                    m_locks.unlock_all(),
                    m_refresh_sources,
                    m_source_progress};
            }

            [[nodiscard]] workspace_detail::PackageRouteContext route_context() {
                return {m_graph,
                    m_packages,
                    materialization_context(),
                    routing_context(),
                    [this](const std::filesystem::path& manifest, std::optional<std::string> name, const SourceLocation& location) {
                        return load_managed(manifest, std::move(name), location);
                    }};
            }

            [[nodiscard]] workspace_detail::DependencyRoutingContext routing_context() const {
                return {m_extensions,
                    m_packages,
                    m_locks.lock(),
                    m_routing,
                    m_locks.mode(),
                    m_context_directory,
                    m_locks.unlocked_packages(),
                    m_locks.unlock_all()};
            }

            Result<void> activate_features(
                const PackageId id,
                const std::span<const std::string> requested,
                const SourceLocation& location
            ) {
                workspace_detail::FeatureActivator activator{m_graph,
                    [&](const PackageId package, const DependencyBinding& dependency) {
                        const PackageNode& requester = m_graph[package];
                        return load_dependency(requester.directory, requester.manifest()->source, dependency);
                    },
                    [&](const PackageId package, const std::string_view member, const SourceLocation& member_location) {
                        const Manifest& manifest = *m_graph[package].manifest();
                        const LocalPackageCandidate* candidate = m_packages.find_for(manifest.source, member);
                        if (!candidate) {
                            return Result<PackageId>{
                                std::unexpected(error_at(member_location, "feature activates unknown member `" + std::string(member) + "`"))
                            };
                        }
                        return load_managed(candidate->manifest, candidate->name, member_location);
                    }};
                return activator.activate(id, requested, location);
            }

            Result<PackageId> load_dependency(
                const std::filesystem::path& source_directory,
                const std::filesystem::path& requester_manifest,
                const DependencyBinding& dependency
            ) {
                auto complete = [&](Result<PackageId> resolved) -> Result<PackageId> {
                    if (!resolved)
                        return std::unexpected(resolved.error());

                    auto activated = activate_features(*resolved, dependency.request.features, dependency.location);
                    if (!activated)
                        return std::unexpected(activated.error());

                    return *resolved;
                };

                const workspace_detail::DependencyRoutingContext context = routing_context();
                auto route = workspace_detail::select_dependency_route(context, requester_manifest, dependency);
                if (!route)
                    return std::unexpected(route.error());

                const workspace_detail::PackageRouteContext routes = route_context();
                if (std::holds_alternative<workspace_detail::PathDependencyRoute>(*route))
                    return complete(workspace_detail::load_path_route(routes, source_directory, dependency));

                if (std::holds_alternative<workspace_detail::SourceDependencyRoute>(*route)) {
                    return complete(
                        workspace_detail::load_source_route(
                            routes,
                            *dependency.selection.source(),
                            source_directory,
                            dependency,
                            std::nullopt,
                            "direct"
                        )
                    );
                }

                if (const auto* provider = std::get_if<workspace_detail::ProviderDependencyRoute>(&*route))
                    return complete(workspace_detail::load_provider_route(routes, provider->provider, source_directory, dependency));

                const auto& local = std::get<workspace_detail::LocalDependencyRoute>(*route);
                return complete(load_managed(local.candidate.manifest, dependency.request.package, dependency.location));
            }

            Graph m_graph;
            PackageIndex m_packages;
            [[nodiscard]] const PolicySchema& policy_schema() const noexcept {
                return m_extensions ? m_extensions->policy_schema() : core_policy_schema();
            }

            ExtensionRegistry* m_extensions = nullptr;
            std::optional<ManifestTree> m_model;
            std::filesystem::path m_source_cache;
            std::span<const ProviderLayer> m_provider_layers;
            const Value* m_feature_settings = nullptr;
            PolicyContext m_policy_context;
            std::map<std::string, std::string> m_routing;
            std::filesystem::path m_context_manifest;
            std::filesystem::path m_context_directory;
            workspace_detail::ResolutionLockCoordinator m_locks;
            bool m_refresh_sources = true;
            std::function<void(std::string_view)> m_source_progress;
            bool m_load_model = true;
            std::span<const std::string> m_excluded_packages;
            bool m_select_package_set = false;
            const ManifestDocument* m_manifest_document = nullptr;
        };
    }

    Result<std::filesystem::path> find_manifest(const std::filesystem::path& start) {
        std::error_code failure;
        std::filesystem::path current = std::filesystem::absolute(start, failure);
        if (failure)
            return std::unexpected(error("cannot resolve path `" + start.string() + "`"));

        if (std::filesystem::is_regular_file(current, failure)) {
            if (current.filename() == "Kaixa.toml")
                return current;

            current = current.parent_path();
        } else if (!std::filesystem::is_directory(current, failure)) {
            return std::unexpected(error("path does not exist: " + current.string()));
        }

        while (!current.empty()) {
            std::filesystem::path candidate = current / "Kaixa.toml";
            if (std::filesystem::is_regular_file(candidate, failure))
                return candidate;

            const std::filesystem::path parent = current.parent_path();
            if (parent == current)
                break;

            current = parent;
        }
        return std::unexpected(error("no Kaixa.toml found from `" + start.string() + "`"));
    }

    Result<Graph> load_workspace(const std::filesystem::path& start, ExtensionRegistry* extensions) {
        ExtensionRegistry registry;
        add_standard_test_adapters(registry);
        ResolutionOptions options;
        options.extensions = extensions ? extensions : &registry;
        auto resolved = resolve_workspace(start, options);
        if (!resolved)
            return std::unexpected(resolved.error());

        return std::move(resolved->graph);
    }

    Result<PackageResolution> resolve_workspace(const std::filesystem::path& start, const std::span<const std::string> selected_packages) {
        ExtensionRegistry registry;
        add_standard_test_adapters(registry);
        ResolutionOptions options;
        options.packages = selected_packages;
        options.extensions = &registry;
        return resolve_workspace(start, options);
    }

    Result<PackageResolution> resolve_workspace(const std::filesystem::path& start, const ResolutionOptions& options) {
        auto manifest = find_manifest(start);
        if (!manifest)
            return std::unexpected(manifest.error());

        std::filesystem::path source_cache = default_source_cache();
        if (!options.source_cache.empty())
            source_cache = options.source_cache;

        WorkspaceLoader loader(options, source_cache);
        return loader.load(*manifest, options.packages);
    }

    Result<PackageResolution> resolve_workspace(
        const std::filesystem::path& manifest,
        const ManifestDocument& document,
        const ResolutionOptions& options
    ) {
        std::filesystem::path source_cache = default_source_cache();
        if (!options.source_cache.empty())
            source_cache = options.source_cache;

        WorkspaceLoader loader(options, source_cache, &document);
        return loader.load(manifest, options.packages);
    }
}
