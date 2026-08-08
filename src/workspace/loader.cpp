#include <kaixa/workspace/loader.hpp>

#include <kaixa/model/manifest.hpp>

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
                    {}
                });

                const std::vector<DependencySpec> dependencies = m_graph[id].manifest->dependencies;
                for (const DependencySpec& dependency: dependencies) {
                    auto target = load_dependency(directory, dependency);
                    if (!target)
                        return std::unexpected(target.error());
                    m_graph[id].dependencies.push_back(*target);
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
