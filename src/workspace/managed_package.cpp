#include "managed_package.hpp"

#include "target_normalization.hpp"

#include <algorithm>
#include <utility>

namespace kaixa::workspace_detail {
    Result<PreparedManagedPackage> prepare_managed_package(
        PackageIndex& packages,
        const std::filesystem::path& directory,
        const std::optional<std::string_view> expected_name,
        const SourceLocation& declaration
    ) {
        const std::filesystem::path manifest_path = directory / "Kaixa.toml";
        auto document = parse_manifest_document_file(manifest_path);
        if (!document)
            return std::unexpected(document.error());

        if (!document->package && document->inline_members.empty())
            return std::unexpected(error_at(declaration, "manifest `" + manifest_path.string() + "` does not declare a package"));

        if (document->package_set) {
            auto included = packages.include(manifest_path);
            if (!included)
                return std::unexpected(included.error());
        }

        std::optional<Manifest> selected;
        if (expected_name) {
            if (document->package && document->package->name == *expected_name)
                selected = std::move(*document->package);

            if (!selected) {
                const auto member = std::ranges::find(document->inline_members, *expected_name, &Manifest::name);
                if (member != document->inline_members.end())
                    selected = std::move(*member);
            }
        } else if (document->package) {
            selected = std::move(*document->package);
        }

        if (!selected) {
            return std::unexpected(error_at(
                declaration,
                expected_name ? "manifest `" + manifest_path.string() + "` does not declare package `" + std::string(*expected_name) + "`"
                              : "manifest `" + manifest_path.string() + "` requires an explicit inline package selection"
            ));
        }
        if (expected_name && selected->name != *expected_name) {
            return std::unexpected(
                error_at(declaration, "dependency `" + std::string(*expected_name) + "` points to package `" + selected->name + "`")
            );
        }

        auto targets = normalize_package_targets(*selected, directory);
        if (!targets)
            return std::unexpected(targets.error());

        return PreparedManagedPackage{manifest_path, std::move(*selected), std::move(*targets)};
    }
}
