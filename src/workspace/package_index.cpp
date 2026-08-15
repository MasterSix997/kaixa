#include <kaixa/workspace/package_index.hpp>

#include <kaixa/config/parser.hpp>
#include <kaixa/model/file_set.hpp>

#include <algorithm>
#include <system_error>
#include <utility>

namespace kaixa {
    namespace {
        Result<std::vector<std::filesystem::path>> expand_members(
            const PackageSet& package_set,
            const std::filesystem::path& directory
        ) {
            FileSet manifests;
            manifests.location = package_set.location;
            manifests.include.reserve(package_set.members.size());
            manifests.exclude.reserve(package_set.exclude.size());
            for (const std::string& member: package_set.members)
                manifests.include.push_back((std::filesystem::path(member) / "Kaixa.toml").generic_string());

            for (const std::string& excluded: package_set.exclude)
                manifests.exclude.push_back((std::filesystem::path(excluded) / "Kaixa.toml").generic_string());

            auto expanded = expand_file_set(manifests, directory, directory);
            if (!expanded)
                return std::unexpected(expanded.error());

            std::vector<std::filesystem::path> result;
            result.reserve(expanded->size());
            for (const std::filesystem::path& relative: *expanded) {
                const std::filesystem::path declared = directory / relative;
                std::error_code failure;
                if (!std::filesystem::is_regular_file(declared, failure)) {
                    return std::unexpected(error_at(
                        package_set.location,
                        failure
                            ? "cannot inspect package set member `" + declared.string() + "`: "
                                + failure.message()
                            : "package set member has no Kaixa.toml: " + declared.string()
                    ));
                }

                std::filesystem::path canonical = std::filesystem::canonical(declared, failure);
                if (failure) {
                    return std::unexpected(error_at(
                        package_set.location,
                        "cannot canonicalize package set member `" + declared.string() + "`: "
                            + failure.message()
                    ));
                }
                result.push_back(std::move(canonical));
            }

            std::ranges::sort(result, {}, [](const std::filesystem::path& path) {
                return path.generic_string();
            });
            result.erase(std::ranges::unique(result).begin(), result.end());
            return result;
        }
    }

    Result<PackageIndex> PackageIndex::discover(
        const std::filesystem::path& selected_manifest,
        const ManifestDocument& selected_document
    ) {
        PackageIndex result;
        std::error_code canonical_failure;
        const std::filesystem::path selected = std::filesystem::canonical(
            selected_manifest,
            canonical_failure
        );
        if (canonical_failure) {
            return std::unexpected(error(
                "cannot canonicalize manifest `" + selected_manifest.string() + "`: "
                    + canonical_failure.message()
            ));
        }

        std::vector<std::filesystem::path> containers;
        std::filesystem::path subject = selected;
        std::filesystem::path directory = selected.parent_path().parent_path();
        while (!directory.empty()) {
            const std::filesystem::path candidate = directory / "Kaixa.toml";
            std::error_code failure;
            const bool exists = std::filesystem::exists(candidate, failure);
            if (failure) {
                return std::unexpected(error(
                    "cannot inspect manifest `" + candidate.string() + "`: " + failure.message()
                ));
            }
            if (exists && std::filesystem::is_regular_file(candidate, failure)) {
                std::filesystem::path canonical = std::filesystem::canonical(candidate, failure);
                if (failure) {
                    return std::unexpected(error(
                        "cannot canonicalize manifest `" + candidate.string() + "`: "
                            + failure.message()
                    ));
                }

                auto raw_document = parse_file(canonical);
                if (!raw_document)
                    return std::unexpected(raw_document.error());
                if (raw_document->find("package-set")) {
                    auto document = parse_manifest_document_file(canonical);
                    if (!document)
                        return std::unexpected(document.error());

                    auto members = expand_members(*document->package_set, canonical.parent_path());
                    if (!members)
                        return std::unexpected(members.error());

                    if (std::ranges::find(*members, subject) != members->end()) {
                        containers.push_back(canonical);
                        subject = canonical;
                    }
                }
            } else if (failure) {
                return std::unexpected(error(
                    "cannot inspect manifest `" + candidate.string() + "`: " + failure.message()
                ));
            }

            const std::filesystem::path parent = directory.parent_path();
            if (parent == directory)
                break;

            directory = parent;
        }

        result.m_context_manifests.assign(containers.rbegin(), containers.rend());
        result.m_context_manifests.push_back(selected);

        if (!containers.empty()) {
            auto indexed = result.index_scope(containers.back(), std::nullopt);
            if (!indexed)
                return std::unexpected(indexed.error());
        } else if (selected_document.package_set) {
            auto indexed = result.index_scope(selected, std::nullopt);
            if (!indexed)
                return std::unexpected(indexed.error());
        }
        return result;
    }

    Result<void> PackageIndex::include(const std::filesystem::path& package_set_manifest) {
        if (m_set_scopes.contains(package_set_manifest))
            return {};

        auto indexed = index_scope(package_set_manifest, std::nullopt);
        if (!indexed)
            return std::unexpected(indexed.error());

        return {};
    }

    Result<void> PackageIndex::add_candidate(
        const std::size_t scope,
        const Manifest& package,
        const std::filesystem::path& manifest
    ) {
        const auto existing = std::ranges::find_if(
            m_candidates,
            [&](const LocalPackageCandidate& candidate) { return candidate.manifest == manifest; }
        );
        std::size_t candidate = 0;
        if (existing == m_candidates.end()) {
            candidate = m_candidates.size();
            m_candidates.push_back({package.name, package.version, manifest, package.location});
        } else {
            candidate = static_cast<std::size_t>(existing - m_candidates.begin());
        }
        return add_candidate(scope, candidate);
    }

    Result<void> PackageIndex::add_candidate(const std::size_t scope, const std::size_t candidate) {
        const LocalPackageCandidate& package = m_candidates[candidate];
        const auto existing = std::ranges::find_if(
            m_scopes[scope].candidates,
            [&](const std::size_t id) { return m_candidates[id].name == package.name; }
        );
        if (existing != m_scopes[scope].candidates.end()) {
            if (*existing == candidate)
                return {};

            return std::unexpected(error_at(
                package.location,
                "package set provides `" + package.name + "` from both `"
                    + m_candidates[*existing].manifest.string() + "` and `"
                    + package.manifest.string() + "`"
            ));
        }

        m_scopes[scope].candidates.push_back(candidate);
        return {};
    }

    Result<std::size_t> PackageIndex::index_scope(
        const std::filesystem::path& manifest_path,
        const std::optional<std::size_t> parent
    ) {
        if (m_indexing.contains(manifest_path)) {
            return std::unexpected(error(
                "package set inclusion cycle reaches `" + manifest_path.string() + "`"
            ));
        }
        if (const auto existing = m_set_scopes.find(manifest_path); existing != m_set_scopes.end()) {
            if (m_scopes[existing->second].parent != parent) {
                return std::unexpected(error(
                    "package set `" + manifest_path.string() + "` is included through more than one scope"
                ));
            }
            return existing->second;
        }

        auto document = parse_manifest_document_file(manifest_path);
        if (!document)
            return std::unexpected(document.error());
        if (!document->package_set) {
            return std::unexpected(error(
                "manifest `" + manifest_path.string() + "` does not declare a package set"
            ));
        }

        const std::size_t id = m_scopes.size();
        m_scopes.push_back({manifest_path, parent, {}});
        m_set_scopes.emplace(manifest_path, id);
        m_indexing.insert(manifest_path);

        if (document->package) {
            auto added = add_candidate(id, *document->package, manifest_path);
            if (!added)
                return std::unexpected(added.error());

            m_package_scopes[manifest_path] = id;
        }

        auto members = expand_members(*document->package_set, manifest_path.parent_path());
        if (!members)
            return std::unexpected(members.error());

        for (const std::filesystem::path& member: *members) {
            if (member == manifest_path) {
                return std::unexpected(error_at(
                    document->package_set->location,
                    "package set cannot include its own manifest"
                ));
            }

            auto child = parse_manifest_document_file(member);
            if (!child)
                return std::unexpected(child.error());

            if (child->package_set) {
                auto child_scope = index_scope(member, id);
                if (!child_scope)
                    return std::unexpected(child_scope.error());

                const std::vector<std::size_t> exported = m_scopes[*child_scope].candidates;
                for (const std::size_t candidate: exported) {
                    auto added = add_candidate(id, candidate);
                    if (!added)
                        return std::unexpected(added.error());
                }
            } else if (child->package) {
                auto added = add_candidate(id, *child->package, member);
                if (!added)
                    return std::unexpected(added.error());

                m_package_scopes[member] = id;
            }
        }

        m_indexing.erase(manifest_path);
        return id;
    }

    const LocalPackageCandidate* PackageIndex::find_for(
        const std::filesystem::path& requester_manifest,
        const std::string_view name
    ) const {
        const auto nearest = m_package_scopes.find(requester_manifest);
        std::optional<std::size_t> scope = nearest == m_package_scopes.end()
            ? std::nullopt
            : std::optional(nearest->second);
        while (scope) {
            const auto candidate = std::ranges::find_if(
                m_scopes[*scope].candidates,
                [&](const std::size_t id) { return m_candidates[id].name == name; }
            );
            if (candidate != m_scopes[*scope].candidates.end())
                return &m_candidates[*candidate];

            scope = m_scopes[*scope].parent;
        }
        return nullptr;
    }

    const LocalPackageCandidate* PackageIndex::find_in_set(
        const std::filesystem::path& package_set_manifest,
        const std::string_view name
    ) const {
        const auto scope = m_set_scopes.find(package_set_manifest);
        if (scope == m_set_scopes.end())
            return nullptr;

        const auto candidate = std::ranges::find_if(
            m_scopes[scope->second].candidates,
            [&](const std::size_t id) { return m_candidates[id].name == name; }
        );
        return candidate == m_scopes[scope->second].candidates.end()
            ? nullptr
            : &m_candidates[*candidate];
    }
}
