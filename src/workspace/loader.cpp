#include <kaixa/workspace/loader.hpp>

#include "dependency_routing.hpp"
#include "feature_activation.hpp"
#include "managed_package.hpp"
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
            WorkspaceLoader(const ResolutionOptions& options, std::filesystem::path source_cache)
                : m_extensions(options.extensions)
                , m_source_cache(std::move(source_cache))
                , m_provider_layers(options.provider_layers)
                , m_feature_settings(options.feature_settings)
                , m_policy_context(options.policy_context)
                , m_lock_mode(options.lock_mode)
                , m_lockfile(options.lockfile)
                , m_unlocked_packages(options.unlocked_packages)
                , m_unlock_all(options.unlock_all)
                , m_write_lock(options.write_lock)
                , m_refresh_sources(options.refresh_sources)
                , m_source_progress(options.source_progress)
                , m_load_model(options.load_model) {}

            Result<PackageResolution> load(
                const std::filesystem::path& manifest_path,
                const std::span<const std::string> selected_packages
            ) {
                auto workspace = open_workspace(manifest_path);
                if (!workspace)
                    return std::unexpected(workspace.error());

                m_model = &workspace->tree;

                auto lock = read_lockfile();
                if (!lock)
                    return std::unexpected(lock.error());

                auto providers = configure_context_providers();
                if (!providers)
                    return std::unexpected(providers.error());

                auto roots = load_selected_packages(workspace->manifest, workspace->document, selected_packages);
                if (selected_packages.empty())
                    roots = load_default_packages(workspace->manifest, workspace->document);

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

                auto instances = configure_package_instances(m_graph, m_policy_context);
                if (!instances)
                    return std::unexpected(instances.error());

                auto lock_changed = update_lockfile(*instances);
                if (!lock_changed)
                    return std::unexpected(lock_changed.error());

                std::vector<std::string> root_names;
                root_names.reserve(m_graph.roots().size());
                for (const PackageId root: m_graph.roots())
                    root_names.push_back(m_graph[root].name);

                std::ranges::sort(root_names);
                ResolutionContext context{m_context_manifest,
                    m_context_directory,
                    m_lockfile,
                    std::move(root_names),
                    m_policy_context,
                    m_lock_mode};

                return PackageResolution{std::move(m_graph),
                    std::move(m_packages),
                    std::move(workspace->document.configurations),
                    std::move(workspace->manifest),
                    std::move(context),
                    *lock_changed,
                    std::move(workspace->tree),
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

                auto document = parse_manifest_document_file(selected);
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
                if (m_lockfile.empty())
                    m_lockfile = m_context_directory / "Kaixa.lock";

                return OpenedWorkspace{selected, std::move(*document), std::move(tree)};
            }

            Result<void> read_lockfile() {
                if (m_lock_mode == LockMode::none)
                    return {};

                auto lock = read_resolution_lock(m_lockfile);
                if (!lock)
                    return std::unexpected(lock.error());

                if (*lock) {
                    m_lock = std::move(**lock);
                    return {};
                }
                if (m_lock_mode == LockMode::locked || m_lock_mode == LockMode::frozen) {
                    return std::unexpected(error("lockfile does not exist: " + m_lockfile.string())
                            .add_note("run the command without `--locked` or `--frozen` to create it"));
                }
                return {};
            }

            Result<bool> update_lockfile(const std::span<const ConfiguredPackageInstance> instances) {
                if (m_lock_mode == LockMode::none)
                    return false;

                const ResolutionLock current = capture_resolution_lock(m_graph, instances, m_policy_context, m_context_directory);
                if (m_lock_mode == LockMode::locked || m_lock_mode == LockMode::frozen) {
                    auto valid = validate_resolution_lock(*m_lock, current);
                    if (!valid)
                        return std::unexpected(valid.error());

                    return false;
                }

                auto before = m_lock ? format_resolution_lock(*m_lock) : Result<std::string>{std::string{}};
                if (!before)
                    return std::unexpected(before.error());

                ResolutionLock merged = m_lock ? merge_resolution_lock(std::move(*m_lock), current) : current;
                if (m_write_lock)
                    return write_resolution_lock(m_lockfile, merged);

                auto after = format_resolution_lock(merged);
                if (!after)
                    return std::unexpected(after.error());

                return *before != *after;
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
                    auto document = parse_manifest_document_file(manifest);
                    if (!document)
                        return std::unexpected(document.error());

                    for (const auto& [package, provider]: document->routing)
                        m_routing[package] = provider;

                    if (!document->providers.empty()) {
                        layers.push_back(
                            {std::move(document->providers),
                                ProviderContext{manifest.parent_path(), m_source_cache, m_lock_mode == LockMode::frozen}}
                        );
                    }
                }
                layers.insert(layers.end(), m_provider_layers.begin(), m_provider_layers.end());
                for (ProviderLayer& layer: layers) {
                    if (layer.context.cache.empty())
                        layer.context.cache = m_source_cache;

                    layer.context.offline = layer.context.offline || m_lock_mode == LockMode::frozen;
                }
                if (layers.empty())
                    return {};

                if (!m_extensions)
                    return std::unexpected(error("provider configuration requires an extension registry"));

                return configure_providers(*m_extensions, layers);
            }

            Result<std::vector<PackageId>> load_default_packages(
                const std::filesystem::path& manifest_path,
                const ManifestDocument& document
            ) {
                if (document.package) {
                    auto root = load_managed(manifest_path, std::nullopt, {});
                    if (!root)
                        return std::unexpected(root.error());

                    return std::vector{*root};
                }
                if (!document.package_set)
                    return std::unexpected(error("manifest does not declare a package"));

                if (document.package_set->defaults.empty()) {
                    return std::unexpected(
                        error_at(document.package_set->location, "package set requires `default` or an explicit package selection")
                    );
                }
                std::vector<PackageId> roots;
                roots.reserve(document.package_set->defaults.size());
                for (const std::string& name: document.package_set->defaults) {
                    const LocalPackageCandidate* candidate = m_packages.find_in_set(manifest_path, name);
                    if (!candidate) {
                        return std::unexpected(
                            error_at(document.package_set->location, "default package `" + name + "` is not a member of the package set")
                        );
                    }

                    auto root = load_managed(candidate->manifest, name, document.package_set->location);
                    if (!root)
                        return std::unexpected(root.error());

                    roots.push_back(*root);
                }
                return roots;
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
                            diagnostic = std::move(diagnostic).add_note("available packages: " + available);
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

                const auto existing = std::ranges::find_if(m_graph.nodes(), [&](const PackageNode& package) {
                    return package.directory == directory && (!expected_name || package.name == *expected_name);
                });
                if (existing != m_graph.nodes().end())
                    return existing->id;

                const std::filesystem::path package_manifest = directory / "Kaixa.toml";
                const ManifestDocument* parsed_document = nullptr;
                if (m_model) {
                    const auto document = std::ranges::find(m_model->documents, package_manifest, &ManifestDocument::source);
                    if (document != m_model->documents.end())
                        parsed_document = &*document;
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

                const PackageId id = m_graph.add(
                    PackageNode{{}, manifest.name, directory, PackageKind::managed, manifest.resolver, std::move(manifest), {}, {}, {}}
                );
                m_graph[id].policy_layers = m_packages.policies_for(canonical_manifest);

                const std::vector<DependencyBinding> dependencies = m_graph[id].manifest->dependencies;
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

                const std::vector<std::string> defaults = m_graph[id].manifest->default_features;
                auto activated = activate_features(id, defaults, m_graph[id].manifest->location);
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

            Result<void> validate_resolved_version(
                const PackageId id,
                const PackageRequest& request,
                const std::optional<Version>& expected,
                const SourceLocation& location
            ) const {
                const std::optional<Manifest>& manifest = m_graph[id].manifest;
                if (!manifest || !manifest->version) {
                    if (request.version || expected) {
                        return std::unexpected(error_at(location, "package `" + request.package + "` does not declare a version"));
                    }
                    return {};
                }

                if (expected && manifest->version != expected) {
                    return std::unexpected(error_at(
                        location,
                        "provider selected `"
                            + request.package
                            + "` version `"
                            + expected->text
                            + "`, but its manifest declares `"
                            + manifest->version->text
                            + "`"
                    ));
                }
                if (request.version && !matches(*request.version, *manifest->version)) {
                    return std::unexpected(error_at(
                        location,
                        "package `"
                            + request.package
                            + "` has version `"
                            + manifest->version->text
                            + "`, which does not satisfy `"
                            + request.version->text
                            + "`"
                    ));
                }
                return {};
            }

            struct SourcePackageResolution {
                std::optional<std::string> provider;
                std::string authority;
                std::optional<std::string> identity;
                std::optional<Version> expected_version;
                std::optional<std::string> integrity;
            };

            std::filesystem::path provider_locator_requester(const SourceLocator& locator, const std::filesystem::path& fallback) {
                const std::string& source = locator.options.location().source;
                if (source.empty())
                    return fallback;

                const std::filesystem::path declaration = source;
                const std::filesystem::path directory = declaration.parent_path();
                return directory.empty() ? fallback : directory;
            }

            bool source_only_candidate(const PackageCandidate& candidate) {
                if (!candidate.descriptor)
                    return false;

                const Value* kind = candidate.descriptor->find("kind");
                const std::string* name = kind ? kind->as_string() : nullptr;
                return name && *name == "source-only";
            }

            Result<std::vector<FeatureDefinition>> adopted_source_features(const PackageCandidate& candidate) {
                if (!candidate.descriptor)
                    return std::vector<FeatureDefinition>{};

                const Value* declared = candidate.descriptor->find("features");
                if (!declared)
                    return std::vector<FeatureDefinition>{};

                const std::vector<Value>* values = declared->as_array();
                if (!values)
                    return std::unexpected(error_at(declared->location(), "adopted source `features` must be an array"));

                std::vector<FeatureDefinition> features;
                features.reserve(values->size());
                for (const Value& value: *values) {
                    const std::string* name = value.as_string();
                    if (!name)
                        return std::unexpected(error_at(value.location(), "adopted source features must be strings"));
                    if (!is_valid_identifier(*name))
                        return std::unexpected(error_at(value.location(), "`" + *name + "` is not a valid feature name"));
                    if (std::ranges::find(features, *name, &FeatureDefinition::name) != features.end())
                        return std::unexpected(error_at(value.location(), "duplicate adopted source feature `" + *name + "`"));

                    features.push_back(FeatureDefinition{.name = *name, .location = value.location()});
                }
                return features;
            }

            Result<std::vector<DependencyBinding>> adopted_source_feature_dependencies(
                const PackageCandidate& candidate,
                std::vector<FeatureDefinition>& features
            ) {
                if (!candidate.descriptor)
                    return std::vector<DependencyBinding>{};

                const Value* declared = candidate.descriptor->find("feature-dependencies");
                if (!declared)
                    return std::vector<DependencyBinding>{};

                const std::vector<TableEntry>* entries = declared->as_table();
                if (!entries)
                    return std::unexpected(error_at(declared->location(), "adopted source `feature-dependencies` must be a table"));

                std::vector<DependencyBinding> dependencies;
                for (const TableEntry& entry: *entries) {
                    const auto feature = std::ranges::find(features, entry.key, &FeatureDefinition::name);
                    if (feature == features.end()) {
                        return std::unexpected(
                            error_at(entry.value.location(), "feature dependencies configure undeclared feature `" + entry.key + "`")
                        );
                    }

                    const std::vector<Value>* names = entry.value.as_array();
                    if (!names) {
                        return std::unexpected(
                            error_at(entry.value.location(), "adopted source feature dependencies must be arrays of package names")
                        );
                    }
                    for (const Value& value: *names) {
                        const std::string* name = value.as_string();
                        if (!name || !is_valid_package_name(*name)) {
                            return std::unexpected(
                                error_at(value.location(), "adopted source feature dependency must be a valid package name")
                            );
                        }

                        feature->dependencies.push_back(*name);
                        if (std::ranges::find_if(
                                dependencies,
                                [&](const DependencyBinding& dependency) { return dependency.request.package == *name; }
                            )
                            != dependencies.end()) {
                            continue;
                        }

                        DependencyBinding dependency;
                        dependency.request.package = *name;
                        dependency.request.optional = true;
                        dependency.location = value.location();
                        dependencies.push_back(std::move(dependency));
                    }
                }
                return dependencies;
            }

            Result<PackageId> load_source_only_dependency(
                const PackageProvider& provider,
                const PackageCandidate& candidate,
                const std::filesystem::path& requester,
                const DependencyBinding& dependency
            ) {
                if (!candidate.source)
                    return std::unexpected(error_at(dependency.location, "source-only package requires a source"));

                const ProviderInfo info = provider.info();
                if (const auto existing = m_graph.find_by_name(candidate.package)) {
                    const std::optional<PackageSource>& resolved = m_graph[*existing].source;
                    if (!resolved
                        || resolved->provider != info.name
                        || resolved->authority != candidate.authority
                        || resolved->version != candidate.version) {
                        return std::unexpected(error_at(
                            dependency.location,
                            "package `" + candidate.package + "` was already resolved to a different provider candidate"
                        ));
                    }
                    return *existing;
                }

                auto materialized = workspace_detail::materialize_source(
                    materialization_context(),
                    *candidate.source,
                    provider_locator_requester(*candidate.source, requester),
                    dependency.request.package,
                    dependency.location,
                    workspace_detail::MaterializationKind::package_source
                );
                if (!materialized)
                    return std::unexpected(materialized.error());

                return m_graph.add(
                    PackageNode{{},
                        candidate.package,
                        std::move(materialized->directory),
                        PackageKind::opaque,
                        {},
                        std::nullopt,
                        {},
                        {},
                        PackageSource{info.name,
                            candidate.authority,
                            candidate.version,
                            candidate.source,
                            std::move(materialized->identity),
                            std::move(materialized->integrity)},
                        candidate.descriptor}
                );
            }

            Result<std::filesystem::path> adopted_source_directory(
                const PackageCandidate& candidate,
                const std::filesystem::path& root,
                const SourceLocation& location
            ) {
                if (!candidate.descriptor)
                    return std::unexpected(error_at(location, "adopted source package has no descriptor"));

                const Value* consumer = candidate.descriptor->find("consumer");
                if (!consumer || !consumer->as_table())
                    return std::unexpected(error_at(location, "adopted source package requires a `consumer` table"));

                const Value* mode_value = consumer->find("mode");
                const std::string* mode = mode_value ? mode_value->as_string() : nullptr;
                if (mode_value && !mode)
                    return std::unexpected(error_at(mode_value->location(), "consumer `mode` must be a string"));
                if (mode && *mode != "add-subdirectory") {
                    return std::unexpected(
                        error_at(mode_value->location(), "adopted source package requires consumer mode `add-subdirectory`")
                    );
                }

                const Value* path_value = consumer->find("path");
                const std::string* path_text = path_value ? path_value->as_string() : nullptr;
                if (path_value && !path_text)
                    return std::unexpected(error_at(path_value->location(), "consumer `path` must be a string"));

                const std::filesystem::path relative = path_text ? std::filesystem::path(*path_text) : std::filesystem::path{"."};
                if (relative.is_absolute() || relative.has_root_path() || std::ranges::find(relative, "..") != relative.end()) {
                    return std::unexpected(error_at(
                        path_value ? path_value->location() : location,
                        "consumer `path` must stay inside the materialized source tree"
                    ));
                }

                return workspace_detail::canonical_directory(root / relative, path_value ? path_value->location() : location);
            }

            Result<PackageId> load_adopted_source_dependency(
                const PackageProvider& provider,
                const PackageCandidate& candidate,
                const std::filesystem::path& requester,
                const DependencyBinding& dependency
            ) {
                if (!candidate.source || !candidate.resolver)
                    return std::unexpected(error_at(dependency.location, "adopted source package requires a source and resolver"));

                const ProviderInfo info = provider.info();
                if (const auto existing = m_graph.find_by_name(candidate.package)) {
                    const std::optional<PackageSource>& resolved = m_graph[*existing].source;
                    if (!resolved
                        || resolved->provider != info.name
                        || resolved->authority != candidate.authority
                        || resolved->version != candidate.version) {
                        return std::unexpected(error_at(
                            dependency.location,
                            "package `" + candidate.package + "` was already resolved to a different provider candidate"
                        ));
                    }
                    return *existing;
                }

                auto materialized = workspace_detail::materialize_source(
                    materialization_context(),
                    *candidate.source,
                    provider_locator_requester(*candidate.source, requester),
                    dependency.request.package,
                    dependency.location,
                    workspace_detail::MaterializationKind::package_source
                );
                if (!materialized)
                    return std::unexpected(materialized.error());

                auto directory = adopted_source_directory(candidate, materialized->directory, dependency.location);
                if (!directory)
                    return std::unexpected(directory.error());

                Manifest manifest{candidate.package, *candidate.resolver};
                manifest.version = candidate.version;
                manifest.source = *directory / "Kaixa.toml";
                manifest.location = dependency.location;
                auto features = adopted_source_features(candidate);
                if (!features)
                    return std::unexpected(features.error());
                auto dependencies = adopted_source_feature_dependencies(candidate, *features);
                if (!dependencies)
                    return std::unexpected(dependencies.error());
                manifest.features = std::move(*features);
                manifest.dependencies = std::move(*dependencies);

                return m_graph.add(
                    PackageNode{{},
                        candidate.package,
                        std::move(*directory),
                        PackageKind::managed,
                        *candidate.resolver,
                        std::move(manifest),
                        {},
                        {},
                        PackageSource{info.name,
                            candidate.authority,
                            candidate.version,
                            candidate.source,
                            std::move(materialized->identity),
                            std::move(materialized->integrity)},
                        candidate.descriptor}
                );
            }

            Result<PackageId> load_package_from_source(
                const std::filesystem::path& directory,
                const SourceLocator& source,
                const DependencyBinding& dependency,
                SourcePackageResolution resolution
            ) {
                const std::filesystem::path manifest_path = directory / "Kaixa.toml";
                auto document = parse_manifest_document_file(manifest_path);
                if (!document)
                    return std::unexpected(document.error());

                std::optional<std::filesystem::path> package_manifest;
                if (document->package && document->package->name == dependency.request.package) {
                    package_manifest = manifest_path;
                } else if (document->package && !document->package_set) {
                    return std::unexpected(error_at(
                        dependency.location,
                        "source points to package `" + document->package->name + "`, not `" + dependency.request.package + "`"
                    ));
                } else if (document->package_set) {
                    auto included = m_packages.include(manifest_path);
                    if (!included)
                        return std::unexpected(included.error());

                    const LocalPackageCandidate* candidate = m_packages.find_in_set(manifest_path, dependency.request.package);
                    if (candidate)
                        package_manifest = candidate->manifest;
                }
                if (!package_manifest) {
                    return std::unexpected(
                        error_at(dependency.location, "source does not provide package `" + dependency.request.package + "`")
                    );
                }

                auto loaded = load_managed(*package_manifest, dependency.request.package, dependency.location);
                if (!loaded)
                    return std::unexpected(loaded.error());

                auto version = validate_resolved_version(*loaded, dependency.request, resolution.expected_version, dependency.location);
                if (!version)
                    return std::unexpected(version.error());

                const std::optional<Version> resolved_version = m_graph[*loaded].manifest ? m_graph[*loaded].manifest->version
                                                                                          : resolution.expected_version;
                m_graph[*loaded].source = PackageSource{std::move(resolution.provider),
                    std::move(resolution.authority),
                    resolved_version,
                    source,
                    std::move(resolution.identity),
                    std::move(resolution.integrity)};
                return *loaded;
            }

            Result<PackageId> load_source_dependency(
                const SourceLocator& source,
                const std::filesystem::path& requester,
                const DependencyBinding& dependency,
                std::optional<std::string> provider,
                std::string authority,
                const std::optional<Version>& expected_version = std::nullopt
            ) {
                auto materialized = workspace_detail::materialize_source(
                    materialization_context(),
                    source,
                    requester,
                    dependency.request.package,
                    dependency.location,
                    workspace_detail::MaterializationKind::package_source
                );
                if (!materialized)
                    return std::unexpected(materialized.error());

                return load_package_from_source(
                    materialized->directory,
                    source,
                    dependency,
                    SourcePackageResolution{std::move(provider),
                        std::move(authority),
                        std::move(materialized->identity),
                        expected_version,
                        std::move(materialized->integrity)}
                );
            }

            Result<PackageId> load_provider_dependency(
                const PackageProvider& provider,
                const std::filesystem::path& requester,
                const DependencyBinding& dependency
            ) {
                auto candidate = workspace_detail::select_provider_candidate(routing_context(), provider, dependency);
                if (!candidate)
                    return std::unexpected(candidate.error());

                const ProviderInfo info = provider.info();
                if (source_only_candidate(*candidate))
                    return load_source_only_dependency(provider, *candidate, requester, dependency);

                if (candidate->source && candidate->resolver && candidate->descriptor && candidate->descriptor->find("consumer")) {
                    return load_adopted_source_dependency(provider, *candidate, requester, dependency);
                }
                if (!candidate->source) {
                    if (const auto existing = m_graph.find_by_name(candidate->package)) {
                        const std::optional<PackageSource>& resolved = m_graph[*existing].source;
                        if (!resolved
                            || resolved->provider != info.name
                            || resolved->authority != candidate->authority
                            || resolved->version != candidate->version) {
                            return std::unexpected(error_at(
                                dependency.location,
                                "package `" + candidate->package + "` was already resolved to a different provider candidate"
                            ));
                        }
                        return *existing;
                    }

                    std::filesystem::path artifact_directory;
                    std::optional<std::string> artifact_identity;
                    std::optional<std::string> artifact_integrity;
                    if (candidate->artifact) {
                        auto materialized = workspace_detail::materialize_source(
                            materialization_context(),
                            *candidate->artifact,
                            provider_locator_requester(*candidate->artifact, requester),
                            dependency.request.package,
                            dependency.location,
                            workspace_detail::MaterializationKind::prebuilt_artifact
                        );
                        if (!materialized)
                            return std::unexpected(materialized.error());

                        artifact_directory = std::move(materialized->directory);
                        artifact_identity = std::move(materialized->identity);
                        artifact_integrity = std::move(materialized->integrity);
                    }

                    return m_graph.add(
                        PackageNode{{},
                            candidate->package,
                            std::move(artifact_directory),
                            PackageKind::opaque,
                            candidate->resolver.value_or(std::string{}),
                            std::nullopt,
                            {},
                            {},
                            PackageSource{info.name,
                                candidate->authority,
                                candidate->version,
                                candidate->artifact,
                                std::move(artifact_identity),
                                std::move(artifact_integrity)},
                            candidate->descriptor}
                    );
                }
                return load_source_dependency(
                    *candidate->source,
                    provider_locator_requester(*candidate->source, requester),
                    dependency,
                    info.name,
                    candidate->authority,
                    candidate->version
                );
            }

            [[nodiscard]] workspace_detail::SourceMaterializationContext materialization_context() const {
                return {m_extensions,
                    m_source_cache,
                    m_lock_mode,
                    m_lock ? &*m_lock : nullptr,
                    m_unlocked_packages,
                    m_unlock_all,
                    m_refresh_sources,
                    m_source_progress};
            }

            [[nodiscard]] workspace_detail::DependencyRoutingContext routing_context() const {
                return {m_extensions,
                    m_packages,
                    m_lock ? &*m_lock : nullptr,
                    m_routing,
                    m_lock_mode,
                    m_context_directory,
                    m_unlocked_packages,
                    m_unlock_all};
            }

            Result<void> activate_features(
                const PackageId id,
                const std::span<const std::string> requested,
                const SourceLocation& location
            ) {
                workspace_detail::FeatureActivator activator{m_graph,
                    [&](const PackageId package, const DependencyBinding& dependency) {
                        const PackageNode& requester = m_graph[package];
                        return load_dependency(requester.directory, requester.manifest->source, dependency);
                    },
                    [&](const PackageId package, const std::string_view member, const SourceLocation& member_location) {
                        const Manifest& manifest = *m_graph[package].manifest;
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

                if (std::holds_alternative<workspace_detail::PathDependencyRoute>(*route))
                    return complete(load_path_dependency(source_directory, dependency));

                if (std::holds_alternative<workspace_detail::SourceDependencyRoute>(*route)) {
                    return complete(
                        load_source_dependency(*dependency.selection.source(), source_directory, dependency, std::nullopt, "direct")
                    );
                }

                if (const auto* provider = std::get_if<workspace_detail::ProviderDependencyRoute>(&*route))
                    return complete(load_provider_dependency(provider->provider, source_directory, dependency));

                const auto& local = std::get<workspace_detail::LocalDependencyRoute>(*route);
                return complete(load_managed(local.candidate.manifest, dependency.request.package, dependency.location));
            }

            Result<PackageId> load_path_dependency(const std::filesystem::path& requester, const DependencyBinding& dependency) {
                const std::filesystem::path& selected_path = *dependency.selection.path();
                auto directory_result = workspace_detail::canonical_directory(requester / selected_path, dependency.location);
                if (!directory_result)
                    return std::unexpected(directory_result.error());

                const std::filesystem::path directory = *directory_result;

                Value path = Value::string(selected_path.generic_string(), dependency.location);
                SourceLocator source{"path", Value::table({{"path", std::move(path)}}, dependency.location)};

                const std::filesystem::path manifest = directory / "Kaixa.toml";
                std::error_code failure;
                if (std::filesystem::is_regular_file(manifest, failure)) {
                    return load_package_from_source(
                        directory,
                        source,
                        dependency,
                        SourcePackageResolution{std::nullopt, "direct", directory.generic_string()}
                    );
                }

                if (const auto existing = m_graph.find_by_directory(directory)) {
                    if (m_graph[*existing].name != dependency.request.package) {
                        return std::unexpected(error_at(
                            dependency.location,
                            "dependency `" + dependency.request.package + "` shares a directory with `" + m_graph[*existing].name + "`"
                        ));
                    }
                    return *existing;
                }

                if (dependency.request.version) {
                    return std::unexpected(error_at(
                        dependency.location,
                        "opaque path dependency `" + dependency.request.package + "` cannot satisfy a version requirement"
                    ));
                }

                if (const auto same_name = m_graph.find_by_name(dependency.request.package)) {
                    return std::unexpected(error_at(
                        dependency.location,
                        "package `"
                            + dependency.request.package
                            + "` is already provided by `"
                            + m_graph[*same_name].directory.string()
                            + "`"
                    ));
                }

                return m_graph.add(
                    PackageNode{{},
                        dependency.request.package,
                        directory,
                        PackageKind::opaque,
                        {},
                        std::nullopt,
                        {},
                        {},
                        PackageSource{std::nullopt, "direct", std::nullopt, std::move(source), directory.generic_string()}}
                );
            }

            Graph m_graph;
            PackageIndex m_packages;
            ExtensionRegistry* m_extensions = nullptr;
            const ManifestTree* m_model = nullptr;
            std::filesystem::path m_source_cache;
            std::span<const ProviderLayer> m_provider_layers;
            const Value* m_feature_settings = nullptr;
            PolicyContext m_policy_context;
            std::map<std::string, std::string> m_routing;
            LockMode m_lock_mode = LockMode::none;
            std::filesystem::path m_lockfile;
            std::filesystem::path m_context_manifest;
            std::filesystem::path m_context_directory;
            std::optional<ResolutionLock> m_lock;
            std::span<const std::string> m_unlocked_packages;
            bool m_unlock_all = false;
            bool m_write_lock = true;
            bool m_refresh_sources = true;
            std::function<void(std::string_view)> m_source_progress;
            bool m_load_model = true;
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

    Result<Graph> load_workspace(const std::filesystem::path& start) {
        ExtensionRegistry registry;
        add_standard_test_adapters(registry);
        ResolutionOptions options;
        options.extensions = &registry;
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
}
