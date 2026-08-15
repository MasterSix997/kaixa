#include <kaixa/workspace/loader.hpp>

#include <kaixa/model/file_set.hpp>
#include <kaixa/model/manifest.hpp>
#include <kaixa/workspace/package_index.hpp>

#include <algorithm>
#include <cctype>
#include <system_error>
#include <utility>

namespace kaixa {
    namespace {
        Result<std::filesystem::path> canonical_directory(const std::filesystem::path& path, const SourceLocation& location = {}) {
            std::error_code failure;
            const bool exists = std::filesystem::exists(path, failure);
            if (failure)
                return std::unexpected(error_at(
                    location,
                    "cannot inspect path `" + path.string() + "`: " + failure.message()
                ));
            if (!exists)
                return std::unexpected(error_at(
                    location,
                    "directory does not exist: " + path.string()
                ));
            if (!std::filesystem::is_directory(path, failure))
                return std::unexpected(error_at(
                    location,
                    failure
                        ? "cannot inspect path `" + path.string() + "`: " + failure.message()
                        : "path is not a directory: " + path.string()
                ));

            std::filesystem::path canonical = std::filesystem::canonical(path, failure);
            if (failure)
                return std::unexpected(error_at(
                    location,
                    "cannot canonicalize directory `" + path.string() + "`: " + failure.message()
                ));
            return canonical;
        }

        std::string target_kind_name(const PackageTargetKind kind) {
            switch (kind) {
                case PackageTargetKind::test: return "test";
                case PackageTargetKind::example: return "example";
                case PackageTargetKind::benchmark: return "benchmark";
            }
            return "target";
        }

        std::string default_target_name(const std::string& package, const PackageTargetKind kind) {
            switch (kind) {
                case PackageTargetKind::test: return package + "_tests";
                case PackageTargetKind::example: return package + "_example";
                case PackageTargetKind::benchmark: return package + "_benchmarks";
            }
            return package + "_target";
        }

        std::string identifier_from_path(std::filesystem::path path) {
            path.replace_extension();
            std::string result = path.generic_string();
            for (char& character: result) {
                const auto byte = static_cast<unsigned char>(character);
                if (!std::isalnum(byte) && character != '_' && character != '-')
                    character = '_';
            }
            return result;
        }

        Result<void> normalize_package_targets(Manifest& manifest, const std::filesystem::path& package_directory) {
            std::vector<PackageTarget> declarations = manifest.targets;
            for (const PackageTargetReference& reference: manifest.target_references) {
                std::filesystem::path declared = reference.path;
                if (declared.filename() != "Kaixa.toml")
                    declared /= "Kaixa.toml";

                FileSet manifests;
                manifests.include.push_back(declared.generic_string());
                manifests.location = reference.location;
                auto files = expand_file_set(manifests, package_directory, package_directory);
                if (!files)
                    return std::unexpected(files.error());

                for (const std::filesystem::path& relative: *files) {
                    auto targets = parse_package_targets_file(package_directory / relative, reference.kind, manifest.resolver);
                    if (!targets)
                        return std::unexpected(targets.error());

                    declarations.insert(
                        declarations.end(),
                        std::make_move_iterator(targets->begin()),
                        std::make_move_iterator(targets->end())
                    );
                }
            }

            std::vector<PackageTarget> normalized;
            for (PackageTarget& declared: declarations) {
                if (!declared.required_features.empty()) {
                    return std::unexpected(error_at(
                        declared.location,
                        "required package features cannot be evaluated yet"
                    ));
                }
                if (declared.kind != PackageTargetKind::test
                    && (declared.discover || !declared.arguments.empty())) {
                    return std::unexpected(error_at(
                        declared.location,
                        "discovery and execution arguments are currently supported only for tests"
                    ));
                }

                const std::filesystem::path source_directory = declared.source.parent_path();
                auto files = expand_file_set(
                    declared.sources,
                    source_directory,
                    package_directory
                );
                if (!files)
                    return std::unexpected(files.error());

                declared.sources.files = std::move(*files);

                if (!declared.each_source) {
                    if (!declared.name)
                        declared.name = default_target_name(manifest.name, declared.kind);

                    normalized.push_back(std::move(declared));
                    continue;
                }

                const std::string prefix = declared.name.value_or(
                    manifest.name + "_" + target_kind_name(declared.kind)
                );
                const std::filesystem::path relative_source_directory =
                    source_directory.lexically_relative(package_directory);
                for (const std::filesystem::path& file: declared.sources.files) {
                    PackageTarget target = declared;
                    target.each_source = false;
                    target.sources.include = {file.generic_string()};
                    target.sources.exclude.clear();
                    target.sources.files = {file};

                    std::filesystem::path local = file.lexically_relative(relative_source_directory);
                    if (local.empty())
                        local = file.filename();

                    target.name = prefix + "_" + identifier_from_path(local);
                    normalized.push_back(std::move(target));
                }
            }

            for (std::size_t index = 0; index < normalized.size(); ++index) {
                const auto duplicate = std::ranges::find_if(
                    normalized.begin(),
                    normalized.begin() + static_cast<std::ptrdiff_t>(index),
                    [&](const PackageTarget& candidate) {
                        return candidate.name == normalized[index].name;
                    }
                );
                if (duplicate != normalized.begin() + static_cast<std::ptrdiff_t>(index)) {
                    return std::unexpected(error_at(
                        normalized[index].location,
                        "duplicate package target `" + *normalized[index].name + "`"
                    ));
                }
            }
            manifest.resolved_targets = std::move(normalized);
            return {};
        }

        class WorkspaceLoader {
        public:
            WorkspaceLoader(
                ExtensionRegistry* extensions,
                std::filesystem::path source_cache,
                const std::span<const ProviderLayer> provider_layers
            ) : m_extensions(extensions),
                m_source_cache(std::move(source_cache)),
                m_provider_layers(provider_layers) {
            }

            Result<PackageResolution> load(const std::filesystem::path& manifest_path, const std::span<const std::string> selected_packages) {
                std::error_code failure;
                const std::filesystem::path selected = std::filesystem::canonical(manifest_path, failure);
                if (failure) {
                    return std::unexpected(error(
                        "cannot canonicalize manifest `" + manifest_path.string() + "`: "
                            + failure.message()
                    ));
                }

                auto document = parse_manifest_document_file(selected);
                if (!document)
                    return std::unexpected(document.error());

                auto packages = PackageIndex::discover(selected, *document);
                if (!packages)
                    return std::unexpected(packages.error());

                m_packages = std::move(*packages);

                auto providers = configure_context_providers();
                if (!providers)
                    return std::unexpected(providers.error());

                auto roots = selected_packages.empty()
                    ? load_default_packages(selected, *document)
                    : load_selected_packages(selected, *document, selected_packages);
                if (!roots)
                    return std::unexpected(roots.error());

                for (const PackageId root: *roots)
                    m_graph.add_root(root);

                auto order = m_graph.build_order();
                if (!order)
                    return std::unexpected(order.error());

                return PackageResolution{
                    std::move(m_graph),
                    std::move(m_packages),
                    std::move(document->configurations),
                    selected
                };
            }

        private:
            Result<void> configure_context_providers() {
                std::vector<ProviderLayer> layers;
                for (const std::filesystem::path& manifest: m_packages.context_manifests()) {
                    auto document = parse_manifest_document_file(manifest);
                    if (!document)
                        return std::unexpected(document.error());
                    if (!document->providers.empty()) {
                        layers.push_back({
                            std::move(document->providers),
                            ProviderContext{manifest.parent_path()}
                        });
                    }
                }
                layers.insert(layers.end(), m_provider_layers.begin(), m_provider_layers.end());
                if (layers.empty())
                    return {};

                if (!m_extensions)
                    return std::unexpected(error("provider configuration requires an extension registry"));

                return configure_providers(*m_extensions, layers);
            }

            Result<std::vector<PackageId>> load_default_packages(const std::filesystem::path& manifest_path, const ManifestDocument& document) {
                if (document.package) {
                    auto root = load_managed(manifest_path, std::nullopt, {});
                    if (!root)
                        return std::unexpected(root.error());

                    return std::vector{*root};
                }
                if (!document.package_set)
                    return std::unexpected(error("manifest does not declare a package"));

                if (document.package_set->defaults.empty()) {
                    return std::unexpected(error_at(
                        document.package_set->location,
                        "package set requires `default` or an explicit package selection"
                    ));
                }
                std::vector<PackageId> roots;
                roots.reserve(document.package_set->defaults.size());
                for (const std::string& name: document.package_set->defaults) {
                    const LocalPackageCandidate* candidate = m_packages.find_in_set(manifest_path, name);
                    if (!candidate) {
                        return std::unexpected(error_at(
                            document.package_set->location,
                            "default package `" + name + "` is not a member of the package set"
                        ));
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
                            return std::unexpected(error(
                                "package `" + name + "` was selected more than once"
                            ));
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
                            diagnostic = std::move(diagnostic).add_note(
                                "available packages: " + available
                            );
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
                auto directory_result = canonical_directory(manifest_path.parent_path(), declaration);
                if (!directory_result)
                    return std::unexpected(directory_result.error());
                const std::filesystem::path directory = *directory_result;

                if (const auto existing = m_graph.find_by_directory(directory)) {
                    if (expected_name && m_graph[*existing].name != *expected_name) {
                        return std::unexpected(error_at(
                            declaration,
                            "dependency `" + std::string(*expected_name)
                                + "` points to package `" + m_graph[*existing].name + "`"
                        ));
                    }
                    return *existing;
                }

                const std::filesystem::path canonical_manifest = directory / "Kaixa.toml";
                auto document = parse_manifest_document_file(canonical_manifest);
                if (!document)
                    return std::unexpected(document.error());
                if (!document->package) {
                    return std::unexpected(error_at(
                        declaration,
                        "manifest `" + canonical_manifest.string() + "` does not declare a package"
                    ));
                }
                if (document->package_set) {
                    auto included = m_packages.include(canonical_manifest);
                    if (!included)
                        return std::unexpected(included.error());
                }
                Manifest manifest = std::move(*document->package);

                if (expected_name && manifest.name != *expected_name) {
                    return std::unexpected(error_at(
                        declaration,
                        "dependency `" + std::string(*expected_name)
                            + "` points to package `" + manifest.name + "`"
                    ));
                }

                auto targets = normalize_package_targets(manifest, directory);
                if (!targets)
                    return std::unexpected(targets.error());

                if (const auto same_name = m_graph.find_by_name(manifest.name)) {
                    return std::unexpected(error_at(
                        manifest.location,
                        "package `" + manifest.name + "` is also provided by `"
                            + m_graph[*same_name].directory.string() + "`"
                    ));
                }

                const PackageId id = m_graph.add(PackageNode{
                    {},
                    manifest.name,
                    directory,
                    PackageKind::managed,
                    manifest.resolver,
                    std::move(manifest),
                    {},
                    {},
                    {}
                });

                const std::vector<DependencyBinding> dependencies = m_graph[id].manifest->dependencies;
                for (const DependencyBinding& dependency: dependencies) {
                    if (dependency.request.optional)
                        continue;

                    auto target = load_dependency(directory, canonical_manifest, dependency);
                    if (!target)
                        return std::unexpected(target.error());

                    m_graph[id].dependencies.push_back(*target);
                }

                const std::vector<PackageTarget> package_targets = m_graph[id].manifest->resolved_targets;
                for (const PackageTarget& package_target: package_targets) {
                    if (package_target.dependencies.empty())
                        continue;

                    PackageTargetDependencies resolved;
                    resolved.target = *package_target.name;
                    resolved.kind = package_target.kind;
                    for (const DependencyBinding& dependency: package_target.dependencies) {
                        if (dependency.request.optional)
                            continue;

                        auto target = load_dependency(
                            package_target.source.parent_path(),
                            canonical_manifest,
                            dependency
                        );
                        if (!target)
                            return std::unexpected(target.error());

                        if (std::ranges::find(resolved.packages, *target) == resolved.packages.end())
                            resolved.packages.push_back(*target);
                    }
                    m_graph[id].target_dependencies.push_back(std::move(resolved));
                }

                return id;
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
                        return std::unexpected(error_at(
                            location,
                            "package `" + request.package + "` does not declare a version"
                        ));
                    }
                    return {};
                }

                if (expected && manifest->version != expected) {
                    return std::unexpected(error_at(
                        location,
                        "provider selected `" + request.package + "` version `"
                            + expected->text + "`, but its manifest declares `"
                            + manifest->version->text + "`"
                    ));
                }
                if (request.version && !matches(*request.version, *manifest->version)) {
                    return std::unexpected(error_at(
                        location,
                        "package `" + request.package + "` has version `"
                            + manifest->version->text + "`, which does not satisfy `"
                            + request.version->text + "`"
                    ));
                }
                return {};
            }

            Result<PackageId> load_package_from_source(
                const std::filesystem::path& directory,
                const SourceLocator& source,
                const DependencyBinding& dependency,
                std::optional<std::string> provider,
                std::string authority,
                std::optional<std::string> identity,
                const std::optional<Version>& expected_version = std::nullopt
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
                        "source points to package `" + document->package->name
                            + "`, not `" + dependency.request.package + "`"
                    ));
                } else if (document->package_set) {
                    auto included = m_packages.include(manifest_path);
                    if (!included)
                        return std::unexpected(included.error());

                    const LocalPackageCandidate* candidate = m_packages.find_in_set(
                        manifest_path,
                        dependency.request.package
                    );
                    if (candidate)
                        package_manifest = candidate->manifest;
                }
                if (!package_manifest) {
                    return std::unexpected(error_at(
                        dependency.location,
                        "source does not provide package `" + dependency.request.package + "`"
                    ));
                }

                auto loaded = load_managed(
                    *package_manifest,
                    dependency.request.package,
                    dependency.location
                );
                if (!loaded)
                    return std::unexpected(loaded.error());

                auto version = validate_resolved_version(
                    *loaded,
                    dependency.request,
                    expected_version,
                    dependency.location
                );
                if (!version)
                    return std::unexpected(version.error());

                m_graph[*loaded].source = PackageSource{
                    std::move(provider),
                    std::move(authority),
                    source,
                    std::move(identity)
                };
                return *loaded;
            }

            Result<PackageId> load_source_dependency(
                const SourceLocator& source,
                const std::filesystem::path& requester,
                const DependencyBinding& dependency,
                std::optional<std::string> provider,
                std::string authority,
                std::optional<Version> expected_version = std::nullopt
            ) {
                SourceDriver* driver = m_extensions
                    ? m_extensions->find_source_driver(source.driver)
                    : nullptr;
                if (!driver) {
                    return std::unexpected(error_at(
                        dependency.location,
                        "source driver `" + source.driver + "` is not installed"
                    ));
                }

                auto located = driver->locate(source, SourceContext{requester, m_source_cache});
                if (!located)
                    return std::unexpected(located.error());

                if (!*located) {
                    return std::unexpected(error_at(
                        dependency.location,
                        "source for package `" + dependency.request.package
                            + "` is not available locally"
                    ).add_note("source synchronization has not been implemented yet"));
                }
                if (!(**located).directory.is_absolute()) {
                    return std::unexpected(error_at(
                        dependency.location,
                        "source driver `" + source.driver + "` returned a relative directory"
                    ));
                }

                auto directory = canonical_directory((**located).directory, dependency.location);
                if (!directory)
                    return std::unexpected(directory.error());

                return load_package_from_source(
                    *directory,
                    source,
                    dependency,
                    std::move(provider),
                    std::move(authority),
                    (**located).identity,
                    expected_version
                );
            }

            Result<PackageCandidate> select_provider_candidate(const PackageProvider& provider, const DependencyBinding& dependency) const {
                auto candidates = provider.candidates(dependency.request);
                if (!candidates)
                    return std::unexpected(candidates.error());

                std::optional<std::size_t> selected;
                bool ambiguous = false;
                for (std::size_t index = 0; index < candidates->size(); ++index) {
                    const PackageCandidate& candidate = (*candidates)[index];
                    if (candidate.package != dependency.request.package) {
                        return std::unexpected(error_at(
                            dependency.location,
                            "provider `" + provider.info().name + "` returned package `"
                                + candidate.package + "` while resolving `"
                                + dependency.request.package + "`"
                        ));
                    }
                    if (candidate.authority.empty()) {
                        return std::unexpected(error_at(
                            dependency.location,
                            "provider `" + provider.info().name
                                + "` returned a candidate without an authority"
                        ));
                    }
                    if (candidate.source.driver.empty()) {
                        return std::unexpected(error_at(
                            dependency.location,
                            "provider `" + provider.info().name
                                + "` returned a candidate without a source driver"
                        ));
                    }

                    auto parsed_version = parse_version(candidate.version.text, dependency.location);
                    if (!parsed_version)
                        return std::unexpected(parsed_version.error());

                    if (dependency.request.version
                        && !matches(*dependency.request.version, candidate.version)) {
                        continue;
                    }

                    if (!selected) {
                        selected = index;
                        ambiguous = false;
                        continue;
                    }

                    const int relation = compare_versions(
                        candidate.version,
                        (*candidates)[*selected].version
                    );
                    if (relation > 0) {
                        selected = index;
                        ambiguous = false;
                    } else if (relation == 0) {
                        ambiguous = true;
                    }
                }

                if (!selected) {
                    return std::unexpected(error_at(
                        dependency.location,
                        "provider `" + provider.info().name + "` has no compatible version of `"
                            + dependency.request.package + "`"
                    ));
                }
                if (ambiguous) {
                    return std::unexpected(error_at(
                        dependency.location,
                        "provider `" + provider.info().name + "` returned multiple candidates for `"
                            + dependency.request.package + "` version `"
                            + (*candidates)[*selected].version.text + "`"
                    ));
                }
                return std::move((*candidates)[*selected]);
            }

            Result<PackageId> load_provider_dependency(
                const PackageProvider& provider,
                const std::filesystem::path& requester,
                const DependencyBinding& dependency
            ) {
                auto candidate = select_provider_candidate(provider, dependency);
                if (!candidate)
                    return std::unexpected(candidate.error());

                const ProviderInfo info = provider.info();
                return load_source_dependency(
                    candidate->source,
                    requester,
                    dependency,
                    info.name,
                    candidate->authority,
                    candidate->version
                );
            }

            Result<const PackageProvider*> default_provider(const SourceLocation& location) const {
                const PackageProvider* selected = nullptr;
                if (m_extensions) {
                    for (const auto& provider: m_extensions->providers()) {
                        if (!provider->info().is_default)
                            continue;

                        if (selected) {
                            return std::unexpected(error_at(
                                location,
                                "more than one default package provider is configured"
                            ));
                        }
                        selected = provider.get();
                    }
                }
                return selected;
            }

            Result<PackageId> load_dependency(
                const std::filesystem::path& source_directory,
                const std::filesystem::path& requester_manifest,
                const DependencyBinding& dependency
            ) {
                if (!dependency.request.features.empty()) {
                    return std::unexpected(error_at(
                        dependency.location,
                        "dependency features cannot be activated yet"
                    ));
                }

                if (dependency.selection.path) {
                    return load_path_dependency(
                        source_directory,
                        dependency
                    );
                }
                if (dependency.selection.source) {
                    return load_source_dependency(
                        *dependency.selection.source,
                        source_directory,
                        dependency,
                        std::nullopt,
                        "direct"
                    );
                }
                if (dependency.selection.provider) {
                    PackageProvider* provider = m_extensions
                        ? m_extensions->find_provider(*dependency.selection.provider)
                        : nullptr;
                    if (!provider) {
                        return std::unexpected(error_at(
                            dependency.location,
                            "provider `" + *dependency.selection.provider + "` is not installed"
                        ));
                    }

                    return load_provider_dependency(*provider, source_directory, dependency);
                }

                const LocalPackageCandidate* candidate = m_packages.find_for(
                    requester_manifest,
                    dependency.request.package
                );
                if (candidate) {
                    if (dependency.request.version) {
                        if (!candidate->version) {
                            return std::unexpected(error_at(
                                dependency.location,
                                "package `" + candidate->name
                                    + "` does not declare a version required by `"
                                    + dependency.request.version->text + "`"
                            ));
                        }
                        if (!matches(*dependency.request.version, *candidate->version)) {
                            return std::unexpected(error_at(
                                dependency.location,
                                "package `" + candidate->name + "` has version `"
                                    + candidate->version->text + "`, which does not satisfy `"
                                    + dependency.request.version->text + "`"
                            ));
                        }
                    }
                    return load_managed(
                        candidate->manifest,
                        dependency.request.package,
                        dependency.location
                    );
                }

                auto provider = default_provider(dependency.location);
                if (!provider)
                    return std::unexpected(provider.error());
                if (*provider)
                    return load_provider_dependency(**provider, source_directory, dependency);

                return std::unexpected(error_at(
                    dependency.location,
                    "no local package or installed provider can resolve `"
                        + dependency.request.package + "`"
                ));
            }

            Result<PackageId> load_path_dependency(const std::filesystem::path& requester, const DependencyBinding& dependency) {
                auto directory_result = canonical_directory(
                    requester / *dependency.selection.path,
                    dependency.location
                );
                if (!directory_result)
                    return std::unexpected(directory_result.error());
                const std::filesystem::path directory = *directory_result;

                Value path = Value::string(
                    dependency.selection.path->generic_string(),
                    dependency.location
                );
                SourceLocator source{
                    "path",
                    Value::table({{"path", std::move(path)}}, dependency.location)
                };

                const std::filesystem::path manifest = directory / "Kaixa.toml";
                std::error_code failure;
                if (std::filesystem::is_regular_file(manifest, failure)) {
                    return load_package_from_source(
                        directory,
                        source,
                        dependency,
                        std::nullopt,
                        "direct",
                        directory.generic_string()
                    );
                }

                if (const auto existing = m_graph.find_by_directory(directory)) {
                    if (m_graph[*existing].name != dependency.request.package) {
                        return std::unexpected(error_at(
                            dependency.location,
                            "dependency `" + dependency.request.package + "` shares a directory with `"
                                + m_graph[*existing].name + "`"
                        ));
                    }
                    return *existing;
                }

                if (dependency.request.version) {
                    return std::unexpected(error_at(
                        dependency.location,
                        "opaque path dependency `" + dependency.request.package
                            + "` cannot satisfy a version requirement"
                    ));
                }

                if (const auto same_name = m_graph.find_by_name(dependency.request.package)) {
                    return std::unexpected(error_at(
                        dependency.location,
                        "package `" + dependency.request.package + "` is already provided by `"
                            + m_graph[*same_name].directory.string() + "`"
                    ));
                }

                return m_graph.add(PackageNode{
                    {},
                    dependency.request.package,
                    directory,
                    PackageKind::opaque,
                    {},
                    std::nullopt,
                    {},
                    {},
                    PackageSource{
                        std::nullopt,
                        "direct",
                        std::move(source),
                        directory.generic_string()
                    }
                });
            }

            Graph m_graph;
            PackageIndex m_packages;
            ExtensionRegistry* m_extensions = nullptr;
            std::filesystem::path m_source_cache;
            std::span<const ProviderLayer> m_provider_layers;
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
        auto resolved = resolve_workspace(start);
        if (!resolved)
            return std::unexpected(resolved.error());

        return std::move(resolved->graph);
    }

    Result<PackageResolution> resolve_workspace(const std::filesystem::path& start, const std::span<const std::string> selected_packages) {
        return resolve_workspace(start, ResolutionOptions{selected_packages, nullptr, {}, {}});
    }

    Result<PackageResolution> resolve_workspace(const std::filesystem::path& start, const ResolutionOptions& options) {
        auto manifest = find_manifest(start);
        if (!manifest)
            return std::unexpected(manifest.error());

        const std::filesystem::path source_cache = options.source_cache.empty()
            ? manifest->parent_path() / ".kaixa" / "sources"
            : options.source_cache;
        WorkspaceLoader loader(options.extensions, source_cache, options.provider_layers);
        return loader.load(*manifest, options.packages);
    }
}
