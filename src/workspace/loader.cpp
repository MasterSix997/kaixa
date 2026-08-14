#include <kaixa/workspace/loader.hpp>

#include <kaixa/model/file_set.hpp>
#include <kaixa/model/manifest.hpp>

#include <algorithm>
#include <cctype>
#include <system_error>
#include <utility>

namespace kaixa {
    namespace {
        Result<std::filesystem::path> canonical_directory(
            const std::filesystem::path& path,
            const SourceLocation& location = {}
        ) {
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
            Result<Graph> load(const std::filesystem::path& manifest_path) {
                auto root = load_managed(manifest_path, std::nullopt, {});
                if (!root)
                    return std::unexpected(root.error());

                auto order = m_graph.build_order();
                if (!order)
                    return std::unexpected(order.error());
                return std::move(m_graph);
            }

        private:
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

                auto manifest_result = parse_manifest_file(manifest_path);
                if (!manifest_result)
                    return std::unexpected(manifest_result.error());
                Manifest manifest = std::move(*manifest_result);

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
                    {}
                });

                const std::vector<DependencySpec> dependencies = m_graph[id].manifest->dependencies;
                for (const DependencySpec& dependency: dependencies) {
                    auto target = load_dependency(directory, dependency);
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
                    for (const DependencySpec& dependency: package_target.dependencies) {
                        auto target = load_dependency(package_target.source.parent_path(), dependency);
                        if (!target)
                            return std::unexpected(target.error());

                        if (std::ranges::find(resolved.packages, *target) == resolved.packages.end())
                            resolved.packages.push_back(*target);
                    }
                    m_graph[id].target_dependencies.push_back(std::move(resolved));
                }

                return id;
            }

            Result<PackageId> load_dependency(
                const std::filesystem::path& requester,
                const DependencySpec& dependency
            ) {
                auto directory_result = canonical_directory(
                    requester / dependency.path,
                    dependency.location
                );
                if (!directory_result)
                    return std::unexpected(directory_result.error());
                const std::filesystem::path directory = *directory_result;

                const std::filesystem::path manifest = directory / "Kaixa.toml";
                std::error_code failure;
                if (std::filesystem::is_regular_file(manifest, failure)) {
                    return load_managed(manifest, dependency.name, dependency.location);
                }

                if (const auto existing = m_graph.find_by_directory(directory)) {
                    if (m_graph[*existing].name != dependency.name) {
                        return std::unexpected(error_at(
                            dependency.location,
                            "dependency `" + dependency.name + "` shares a directory with `"
                                + m_graph[*existing].name + "`"
                        ));
                    }
                    return *existing;
                }

                if (const auto same_name = m_graph.find_by_name(dependency.name)) {
                    return std::unexpected(error_at(
                        dependency.location,
                        "package `" + dependency.name + "` is already provided by `"
                            + m_graph[*same_name].directory.string() + "`"
                    ));
                }

                return m_graph.add(PackageNode{
                    {},
                    dependency.name,
                    directory,
                    PackageKind::opaque,
                    {},
                    std::nullopt,
                    {},
                    {}
                });
            }

            Graph m_graph;
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
        auto manifest = find_manifest(start);
        if (!manifest)
            return std::unexpected(manifest.error());
        WorkspaceLoader loader;
        return loader.load(*manifest);
    }
}
