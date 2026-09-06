#include "package_routes.hpp"

#include <kaixa/model/graph.hpp>

#include <algorithm>
#include <system_error>
#include <utility>

namespace kaixa::workspace_detail {
    namespace {
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

        Result<std::optional<PackageId>> existing_provider_candidate(
            const Graph& graph,
            const PackageProvider& provider,
            const PackageCandidate& candidate,
            const SourceLocation& location
        ) {
            const auto existing = graph.find_by_name(candidate.package);
            if (!existing)
                return std::nullopt;

            const ProviderInfo info = provider.info();
            const std::optional<PackageSource>& resolved = graph[*existing].source;
            if (!resolved
                || resolved->provider != info.name
                || resolved->authority != candidate.authority
                || resolved->version != candidate.version) {
                return std::unexpected(
                    error_at(location, "package `" + candidate.package + "` was already resolved to a different provider candidate")
                );
            }
            return existing;
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

        Result<PackageId> load_source_only_package(
            const PackageRouteContext& context,
            const PackageProvider& provider,
            const PackageCandidate& candidate,
            const std::filesystem::path& requester,
            const DependencyBinding& dependency
        ) {
            if (!candidate.source)
                return std::unexpected(error_at(dependency.location, "source-only package requires a source"));

            const ProviderInfo info = provider.info();
            auto existing = existing_provider_candidate(context.graph, provider, candidate, dependency.location);
            if (!existing)
                return std::unexpected(existing.error());

            if (*existing)
                return **existing;

            auto materialized = materialize_source(
                context.sources,
                *candidate.source,
                provider_locator_requester(*candidate.source, requester),
                dependency.request.package,
                dependency.location,
                MaterializationKind::package_source
            );
            if (!materialized)
                return std::unexpected(materialized.error());

            PackageNode node;
            node.name = candidate.package;
            node.directory = std::move(materialized->directory);
            node.source = PackageSource{info.name,
                candidate.authority,
                candidate.version,
                candidate.source,
                std::move(materialized->identity),
                std::move(materialized->integrity)};
            node.semantics = OpaquePackage{candidate.descriptor};
            return context.graph.add(std::move(node));
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

            return canonical_directory(root / relative, path_value ? path_value->location() : location);
        }

        Result<PackageId> load_adopted_source_package(
            const PackageRouteContext& context,
            const PackageProvider& provider,
            const PackageCandidate& candidate,
            const std::filesystem::path& requester,
            const DependencyBinding& dependency
        ) {
            if (!candidate.source || !candidate.resolver)
                return std::unexpected(error_at(dependency.location, "adopted source package requires a source and resolver"));

            const ProviderInfo info = provider.info();
            auto existing = existing_provider_candidate(context.graph, provider, candidate, dependency.location);
            if (!existing)
                return std::unexpected(existing.error());

            if (*existing)
                return **existing;

            auto materialized = materialize_source(
                context.sources,
                *candidate.source,
                provider_locator_requester(*candidate.source, requester),
                dependency.request.package,
                dependency.location,
                MaterializationKind::package_source
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

            PackageNode node;
            node.name = candidate.package;
            node.directory = std::move(*directory);
            node.resolver = *candidate.resolver;
            node.source = PackageSource{info.name,
                candidate.authority,
                candidate.version,
                candidate.source,
                std::move(materialized->identity),
                std::move(materialized->integrity)};
            node.semantics = AdoptedPackage{std::move(manifest), *candidate.descriptor};
            return context.graph.add(std::move(node));
        }

        Result<void> validate_resolved_version(
            const Graph& graph,
            const PackageId id,
            const PackageRequest& request,
            const std::optional<Version>& expected,
            const SourceLocation& location
        ) {
            const Manifest* manifest = graph[id].manifest();
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

        Result<PackageId> load_package_from_source(
            const PackageRouteContext& context,
            const std::filesystem::path& directory,
            const SourceLocator& source,
            const DependencyBinding& dependency,
            SourcePackageResolution resolution
        ) {
            const std::filesystem::path manifest_path = directory / "Kaixa.toml";
            auto document = context.packages.load_document(manifest_path);
            if (!document)
                return std::unexpected(document.error());

            const ManifestDocument& source_document = **document;
            std::optional<std::filesystem::path> package_manifest;
            if (source_document.package && source_document.package->name == dependency.request.package) {
                package_manifest = manifest_path;
            } else if (source_document.package && !source_document.package_set) {
                return std::unexpected(error_at(
                    dependency.location,
                    "source points to package `" + source_document.package->name + "`, not `" + dependency.request.package + "`"
                ));
            } else if (source_document.package_set) {
                auto included = context.packages.include(manifest_path);
                if (!included)
                    return std::unexpected(included.error());

                const LocalPackageCandidate* candidate = context.packages.find_in_set(manifest_path, dependency.request.package);
                if (candidate)
                    package_manifest = candidate->manifest;
            }
            if (!package_manifest) {
                return std::unexpected(
                    error_at(dependency.location, "source does not provide package `" + dependency.request.package + "`")
                );
            }

            auto loaded = context.load_managed(*package_manifest, dependency.request.package, dependency.location);
            if (!loaded)
                return std::unexpected(loaded.error());

            auto version = validate_resolved_version(
                context.graph,
                *loaded,
                dependency.request,
                resolution.expected_version,
                dependency.location
            );
            if (!version)
                return std::unexpected(version.error());

            const Manifest* loaded_manifest = context.graph[*loaded].manifest();
            const std::optional<Version> resolved_version = loaded_manifest ? loaded_manifest->version : resolution.expected_version;
            context.graph[*loaded].source = PackageSource{std::move(resolution.provider),
                std::move(resolution.authority),
                resolved_version,
                source,
                std::move(resolution.identity),
                std::move(resolution.integrity)};
            return *loaded;
        }
    }

    Result<PackageId> load_source_route(
        const PackageRouteContext& context,
        const SourceLocator& source,
        const std::filesystem::path& requester,
        const DependencyBinding& dependency,
        std::optional<std::string> provider,
        std::string authority,
        const std::optional<Version>& expected_version
    ) {
        auto materialized = materialize_source(
            context.sources,
            source,
            requester,
            dependency.request.package,
            dependency.location,
            MaterializationKind::package_source
        );
        if (!materialized)
            return std::unexpected(materialized.error());

        return load_package_from_source(
            context,
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

    Result<PackageId> load_provider_route(
        const PackageRouteContext& context,
        const PackageProvider& provider,
        const std::filesystem::path& requester,
        const DependencyBinding& dependency
    ) {
        auto candidate = select_provider_candidate(context.routing, provider, dependency);
        if (!candidate)
            return std::unexpected(candidate.error());

        const ProviderInfo info = provider.info();
        if (source_only_candidate(*candidate))
            return load_source_only_package(context, provider, *candidate, requester, dependency);

        if (candidate->source && candidate->resolver && candidate->descriptor && candidate->descriptor->find("consumer")) {
            return load_adopted_source_package(context, provider, *candidate, requester, dependency);
        }
        if (!candidate->source) {
            auto existing = existing_provider_candidate(context.graph, provider, *candidate, dependency.location);
            if (!existing)
                return std::unexpected(existing.error());

            if (*existing)
                return **existing;

            std::filesystem::path artifact_directory;
            std::optional<std::string> artifact_identity;
            std::optional<std::string> artifact_integrity;
            if (candidate->artifact) {
                auto materialized = materialize_source(
                    context.sources,
                    *candidate->artifact,
                    provider_locator_requester(*candidate->artifact, requester),
                    dependency.request.package,
                    dependency.location,
                    MaterializationKind::prebuilt_artifact
                );
                if (!materialized)
                    return std::unexpected(materialized.error());

                artifact_directory = std::move(materialized->directory);
                artifact_identity = std::move(materialized->identity);
                artifact_integrity = std::move(materialized->integrity);
            }

            PackageNode node;
            node.name = candidate->package;
            node.directory = std::move(artifact_directory);
            node.resolver = candidate->resolver.value_or(std::string{});
            node.source = PackageSource{info.name,
                candidate->authority,
                candidate->version,
                candidate->artifact,
                std::move(artifact_identity),
                std::move(artifact_integrity)};
            node.semantics = OpaquePackage{candidate->descriptor};
            return context.graph.add(std::move(node));
        }
        return load_source_route(
            context,
            *candidate->source,
            provider_locator_requester(*candidate->source, requester),
            dependency,
            info.name,
            candidate->authority,
            candidate->version
        );
    }

    Result<PackageId> load_path_route(
        const PackageRouteContext& context,
        const std::filesystem::path& requester,
        const DependencyBinding& dependency
    ) {
        const std::filesystem::path& selected_path = *dependency.selection.path();
        auto directory_result = canonical_directory(requester / selected_path, dependency.location);
        if (!directory_result)
            return std::unexpected(directory_result.error());

        const std::filesystem::path directory = *directory_result;

        Value path = Value::string(selected_path.generic_string(), dependency.location);
        SourceLocator source{"path", Value::table({{"path", std::move(path)}}, dependency.location)};

        const std::filesystem::path manifest = directory / "Kaixa.toml";
        std::error_code failure;
        if (std::filesystem::is_regular_file(manifest, failure)) {
            return load_package_from_source(
                context,
                directory,
                source,
                dependency,
                SourcePackageResolution{std::nullopt, "direct", directory.generic_string()}
            );
        }

        if (const auto existing = context.graph.find_by_directory(directory)) {
            if (context.graph[*existing].name != dependency.request.package) {
                return std::unexpected(error_at(
                    dependency.location,
                    "dependency `" + dependency.request.package + "` shares a directory with `" + context.graph[*existing].name + "`"
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

        if (const auto same_name = context.graph.find_by_name(dependency.request.package)) {
            return std::unexpected(error_at(
                dependency.location,
                "package `" + dependency.request.package + "` is already provided by `" + context.graph[*same_name].directory.string() + "`"
            ));
        }

        PackageNode node;
        node.name = dependency.request.package;
        node.directory = directory;
        node.source = PackageSource{std::nullopt, "direct", std::nullopt, std::move(source), directory.generic_string()};
        return context.graph.add(std::move(node));
    }
}
