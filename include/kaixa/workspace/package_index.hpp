#pragma once

#include <kaixa/model/manifest.hpp>

#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kaixa {
    struct LocalPackageCandidate {
        std::string name;
        std::optional<Version> version;
        std::filesystem::path manifest;
        SourceLocation location;
    };

    class PackageIndex {
      public:
        [[nodiscard]] static Result<PackageIndex>
        discover(const std::filesystem::path& selected_manifest, const ManifestDocument& selected_document);

        [[nodiscard]] Result<void> include(const std::filesystem::path& package_set_manifest);
        [[nodiscard]] const LocalPackageCandidate*
        find_for(const std::filesystem::path& requester_manifest, std::string_view name) const;
        [[nodiscard]] const LocalPackageCandidate*
        find_in_set(const std::filesystem::path& package_set_manifest, std::string_view name) const;
        [[nodiscard]] std::span<const LocalPackageCandidate> candidates() const noexcept {
            return m_candidates;
        }
        [[nodiscard]] std::span<const std::filesystem::path> context_manifests() const noexcept {
            return m_context_manifests;
        }
        [[nodiscard]] std::vector<Value> policies_for(const std::filesystem::path& requester_manifest) const;

      private:
        struct Scope {
            std::filesystem::path manifest;
            std::optional<std::size_t> parent;
            std::vector<std::size_t> candidates;
            std::optional<Value> policy;
        };

        [[nodiscard]] Result<std::size_t>
        index_scope(const std::filesystem::path& manifest, std::optional<std::size_t> parent);
        [[nodiscard]] Result<void>
        add_candidate(std::size_t scope, const Manifest& package, const std::filesystem::path& manifest);
        [[nodiscard]] Result<void> add_candidate(std::size_t scope, std::size_t candidate);

        std::vector<LocalPackageCandidate> m_candidates;
        std::vector<std::filesystem::path> m_context_manifests;
        std::vector<Scope> m_scopes;
        std::map<std::filesystem::path, std::size_t> m_set_scopes;
        std::map<std::filesystem::path, std::size_t> m_package_scopes;
        std::set<std::filesystem::path> m_indexing;
    };
}
