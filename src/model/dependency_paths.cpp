#include <kaixa/model/dependency_paths.hpp>

#include <algorithm>
#include <string>

namespace kaixa {
    Result<const PackageNode*> find_dependency_by_local_name(
        const Graph& graph,
        const PackageNode& package,
        const std::string_view local_name,
        const SourceLocation& location
    ) {
        if (!package.manifest())
            return std::unexpected(error_at(location, "package `" + package.name + "` has no manifest to resolve dependencies from"));

        const auto binding = std::ranges::find_if(package.manifest()->dependencies, [&](const DependencyBinding& item) {
            return item.local_name() == local_name;
        });
        if (binding == package.manifest()->dependencies.end()) {
            return std::unexpected(error_at(location, "unknown dependency `" + std::string(local_name) + "` in product path"));
        }
        const auto dependency = std::ranges::find_if(package.dependencies, [&](const PackageId id) {
            return graph[id].name == binding->request.package;
        });
        if (dependency == package.dependencies.end()) {
            return std::unexpected(error_at(location, "dependency `" + std::string(local_name) + "` is not active for this product"));
        }
        return &graph[*dependency];
    }

    Result<std::filesystem::path> dependency_source_directory(const PackageNode& dependency, const SourceLocation& location) {
        if (!dependency.directory.empty())
            return dependency.directory;

        if (dependency.descriptor()) {
            const Value* source = dependency.descriptor()->find("source");
            const Value* driver = source ? source->find("driver") : nullptr;
            const Value* path = source ? source->find("path") : nullptr;
            const std::string* driver_name = driver ? driver->as_string() : nullptr;
            const std::string* declared_path = path ? path->as_string() : nullptr;
            if (driver_name && *driver_name == "path" && declared_path) {
                std::filesystem::path resolved = *declared_path;
                if (resolved.is_relative() && !path->location().source.empty()) {
                    resolved = std::filesystem::path(path->location().source).parent_path() / resolved;
                }
                return std::filesystem::absolute(resolved).lexically_normal();
            }
        }
        return std::unexpected(error_at(location, "dependency `" + dependency.name + "` has no materialized source directory"));
    }
}
