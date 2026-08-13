#include <kaixa/services/clean_service.hpp>

#include <algorithm>
#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

namespace kaixa {
    namespace {
        Result<std::filesystem::path> absolute_normalized(const std::filesystem::path& path) {
            std::error_code failure;
            std::filesystem::path result = std::filesystem::absolute(path, failure);
            if (failure) {
                return std::unexpected(error(
                    "cannot resolve path `" + path.string() + "`: " + failure.message()
                ));
            }

            return result.lexically_normal();
        }

        bool is_descendant(const std::filesystem::path& path, const std::filesystem::path& root) {
            const std::filesystem::path relative = path.lexically_relative(root);
            if (relative.empty() || relative == "." || relative.is_absolute())
                return false;

            for (const std::filesystem::path& component: relative) {
                if (component == "..")
                    return false;
            }
            return true;
        }

        std::size_t path_depth(const std::filesystem::path& path) {
            return static_cast<std::size_t>(std::distance(path.begin(), path.end()));
        }
    }

    void CleanPlan::add(std::filesystem::path path) {
        path = path.lexically_normal();
        if (std::ranges::find(m_paths, path) == m_paths.end())
            m_paths.push_back(std::move(path));
    }

    void CleanPlan::generated_file(GeneratedCleanFile file) {
        file.path = file.path.lexically_normal();
        const auto existing = std::ranges::find_if(
            m_generated_files,
            [&](const GeneratedCleanFile& candidate) {
                return candidate.path == file.path;
            }
        );
        if (existing == m_generated_files.end())
            m_generated_files.push_back(std::move(file));
    }

    Result<CleanPlan> plan_clean(
        const Graph& graph,
        const ResolverRegistry& registry,
        const BuildEnvironment& environment,
        const CleanRequest& request
    ) {
        CleanPlan plan;
        for (const PackageNode& package: graph.nodes()) {
            if (package.kind == PackageKind::opaque)
                continue;

            Resolver* resolver = registry.find(package.resolver);
            if (!resolver) {
                return std::unexpected(error(
                    "resolver `" + package.resolver + "` is not installed"
                ));
            }

            auto planned = resolver->plan_clean(
                graph,
                package,
                environment,
                request,
                plan
            );
            if (!planned)
                return std::unexpected(planned.error());
        }
        return plan;
    }

    Result<CleanReport> clean(
        const CleanPlan& plan,
        const std::filesystem::path& state_root,
        const std::filesystem::path& workspace,
        const bool dry_run,
        const bool allow_state_root
    ) {
        auto normalized_root = absolute_normalized(state_root);
        if (!normalized_root)
            return std::unexpected(normalized_root.error());
        auto normalized_workspace = absolute_normalized(workspace);
        if (!normalized_workspace)
            return std::unexpected(normalized_workspace.error());

        std::vector<std::filesystem::path> paths;
        paths.reserve(plan.paths().size());
        for (const std::filesystem::path& path: plan.paths()) {
            auto normalized = absolute_normalized(path);
            if (!normalized)
                return std::unexpected(normalized.error());

            const bool is_root = *normalized == *normalized_root;
            if ((!is_root && !is_descendant(*normalized, *normalized_root))
                || (is_root && !allow_state_root)) {
                return std::unexpected(error(
                    "refusing to clean path outside the selected Kaixa state: "
                        + normalized->string()
                ));
            }

            if (std::ranges::find(paths, *normalized) == paths.end())
                paths.push_back(std::move(*normalized));
        }

        std::ranges::sort(paths, [](const auto& left, const auto& right) {
            return path_depth(left) > path_depth(right);
        });

        std::vector<std::filesystem::path> generated_paths;
        generated_paths.reserve(plan.generated_files().size());
        for (const GeneratedCleanFile& generated: plan.generated_files()) {
            auto normalized = absolute_normalized(generated.path);
            if (!normalized)
                return std::unexpected(normalized.error());
            if (!is_descendant(*normalized, *normalized_workspace)) {
                return std::unexpected(error(
                    "refusing to clean generated file outside the workspace: "
                        + normalized->string()
                ));
            }

            std::error_code failure;
            const bool exists = std::filesystem::exists(*normalized, failure);
            if (failure) {
                return std::unexpected(error(
                    "cannot inspect generated file `" + normalized->string() + "`: "
                        + failure.message()
                ));
            }
            if (!exists)
                continue;
            if (!std::filesystem::is_regular_file(*normalized, failure) || failure) {
                return std::unexpected(error(
                    "generated clean path is not a regular file: " + normalized->string()
                ));
            }

            std::ifstream input(*normalized, std::ios::binary);
            std::string first_line;
            if (!input || !std::getline(input, first_line)) {
                return std::unexpected(error(
                    "cannot inspect generated file `" + normalized->string() + "`"
                ));
            }
            if (!first_line.empty() && first_line.back() == '\r')
                first_line.pop_back();

            if (first_line != generated.marker) {
                return std::unexpected(error(
                    "refusing to clean `" + normalized->string()
                        + "` because it was not generated by Kaixa"
                ));
            }
            generated_paths.push_back(std::move(*normalized));
        }

        CleanReport report;
        for (const std::filesystem::path& path: paths) {
            std::error_code failure;
            const bool exists = std::filesystem::exists(path, failure);
            if (failure) {
                return std::unexpected(error(
                    "cannot inspect clean path `" + path.string() + "`: "
                        + failure.message()
                ));
            }
            if (!exists || dry_run)
                continue;

            const std::uintmax_t removed = std::filesystem::remove_all(path, failure);
            if (failure) {
                return std::unexpected(error(
                    "cannot remove `" + path.string() + "`: " + failure.message()
                ));
            }

            ++report.removed_paths;
            report.removed_entries += removed;
        }

        if (dry_run)
            return report;

        for (const std::filesystem::path& path: generated_paths) {
            std::error_code failure;
            if (!std::filesystem::remove(path, failure) || failure) {
                return std::unexpected(error(
                    "cannot remove generated file `" + path.string() + "`: "
                        + failure.message()
                ));
            }

            ++report.removed_paths;
            ++report.removed_entries;
        }
        return report;
    }
}
