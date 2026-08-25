#include "dependency_routing.hpp"

#include <kaixa/model/version.hpp>

namespace kaixa::workspace_detail {
    namespace {
        Result<PackageProvider*> default_provider(ExtensionRegistry* extensions, const SourceLocation& location) {
            PackageProvider* selected = nullptr;
            if (!extensions)
                return selected;

            for (const auto& provider: extensions->providers()) {
                if (!provider->info().is_default)
                    continue;

                if (selected)
                    return std::unexpected(error_at(location, "more than one default package provider is configured"));

                selected = provider.get();
            }
            return selected;
        }

        Result<void> validate_local_candidate(const LocalPackageCandidate& candidate, const DependencyBinding& dependency) {
            if (!dependency.request.version)
                return {};

            if (!candidate.version) {
                return std::unexpected(error_at(
                    dependency.location,
                    "package `" + candidate.name + "` does not declare a version required by `" + dependency.request.version->text + "`"
                ));
            }
            if (!matches(*dependency.request.version, *candidate.version)) {
                return std::unexpected(error_at(
                    dependency.location,
                    "package `"
                        + candidate.name
                        + "` has version `"
                        + candidate.version->text
                        + "`, which does not satisfy `"
                        + dependency.request.version->text
                        + "`"
                ));
            }
            return {};
        }

        PackageProvider* find_provider(ExtensionRegistry* extensions, const std::string_view name) {
            return extensions ? extensions->find_provider(name) : nullptr;
        }
    }

    namespace {
        struct ProviderCandidateSearch {
            std::optional<std::size_t> best;
            std::optional<std::size_t> locked;
            bool ambiguous = false;
        };

        Result<bool> provider_candidate_is_compatible(
            const PackageCandidate& candidate,
            const PackageRequest& request,
            const std::string_view provider,
            const SourceLocation& location
        ) {
            if (candidate.package != request.package) {
                return std::unexpected(error_at(
                    location,
                    "provider `"
                        + std::string(provider)
                        + "` returned package `"
                        + candidate.package
                        + "` while resolving `"
                        + request.package
                        + "`"
                ));
            }
            if (candidate.authority.empty()) {
                return std::unexpected(
                    error_at(location, "provider `" + std::string(provider) + "` returned a candidate without an authority")
                );
            }
            if ((candidate.source && candidate.source->driver.empty()) || (candidate.artifact && candidate.artifact->driver.empty())) {
                return std::unexpected(
                    error_at(location, "provider `" + std::string(provider) + "` returned a candidate without a source driver")
                );
            }

            if (candidate.version) {
                auto parsed_version = parse_version(candidate.version->text, location);
                if (!parsed_version)
                    return std::unexpected(parsed_version.error());

                return !request.version || matches(*request.version, *candidate.version);
            }
            return !request.version || request.version->text == "*";
        }

        Result<void> consider_provider_candidate(
            ProviderCandidateSearch& search,
            const std::vector<PackageCandidate>& candidates,
            const std::size_t index,
            const LockedPackage* locked,
            const std::string_view provider,
            const std::filesystem::path& context_directory,
            const SourceLocation& location
        ) {
            const PackageCandidate& candidate = candidates[index];
            if (locked && locked_candidate_matches(*locked, provider, candidate, context_directory)) {
                if (search.locked) {
                    return std::unexpected(error_at(
                        location,
                        "provider `"
                            + std::string(provider)
                            + "` returned the locked candidate more than once for `"
                            + candidate.package
                            + "`"
                    ));
                }
                search.locked = index;
            }

            if (!search.best) {
                search.best = index;
                search.ambiguous = false;
                return {};
            }

            const PackageCandidate& best = candidates[*search.best];
            if (!candidate.version && !best.version) {
                search.ambiguous = true;
                return {};
            }
            if (!candidate.version)
                return {};

            if (!best.version) {
                search.best = index;
                search.ambiguous = false;
                return {};
            }

            const int relation = compare_versions(*candidate.version, *best.version);
            if (relation > 0) {
                search.best = index;
                search.ambiguous = false;
            } else if (relation == 0) {
                search.ambiguous = true;
            }
            return {};
        }
    }

    Result<DependencyRoute> select_dependency_route(
        const DependencyRoutingContext& context,
        const std::filesystem::path& requester_manifest,
        const DependencyBinding& dependency
    ) {
        if (dependency.selection.path())
            return PathDependencyRoute{};

        if (dependency.selection.source())
            return SourceDependencyRoute{};

        if (const std::string* selected_provider = dependency.selection.provider()) {
            PackageProvider* provider = find_provider(context.extensions, *selected_provider);
            if (!provider) {
                return std::unexpected(error_at(dependency.location, "provider `" + *selected_provider + "` is not installed"));
            }
            return ProviderDependencyRoute{*provider};
        }

        const LocalPackageCandidate* local = context.packages.find_for(requester_manifest, dependency.request.package);
        if (local) {
            auto valid = validate_local_candidate(*local, dependency);
            if (!valid)
                return std::unexpected(valid.error());

            return LocalDependencyRoute{*local};
        }

        const bool unlocked = context.unlock_all
            || std::ranges::find(context.unlocked_packages, dependency.request.package) != context.unlocked_packages.end();
        const LockedPackage* locked = context.lock && !unlocked ? context.lock->find(dependency.request.package) : nullptr;
        if (locked && locked->provider) {
            PackageProvider* provider = find_provider(context.extensions, *locked->provider);
            if (provider)
                return ProviderDependencyRoute{*provider};

            if (context.lock_mode == LockMode::locked || context.lock_mode == LockMode::frozen) {
                return std::unexpected(
                    error_at(dependency.location, "provider `" + *locked->provider + "` pinned by Kaixa.lock is not installed")
                );
            }
        }

        const auto exact = context.routing.find(dependency.request.package);
        const auto wildcard = context.routing.find("*");
        const auto route = exact != context.routing.end() ? exact : wildcard;
        if (route != context.routing.end()) {
            PackageProvider* provider = find_provider(context.extensions, route->second);
            if (!provider) {
                return std::unexpected(
                    error_at(dependency.location, "provider `" + route->second + "` selected by routing is not installed")
                );
            }
            return ProviderDependencyRoute{*provider};
        }

        auto provider = default_provider(context.extensions, dependency.location);
        if (!provider)
            return std::unexpected(provider.error());

        if (*provider)
            return ProviderDependencyRoute{**provider};

        return std::unexpected(
            error_at(dependency.location, "no local package or installed provider can resolve `" + dependency.request.package + "`")
        );
    }

    Result<PackageCandidate> select_provider_candidate(
        const DependencyRoutingContext& context,
        const PackageProvider& provider,
        const DependencyBinding& dependency
    ) {
        auto candidates = provider.candidates(dependency.request);
        if (!candidates)
            return std::unexpected(candidates.error());

        const ProviderInfo provider_info = provider.info();
        const bool unlocked = context.unlock_all
            || std::ranges::find(context.unlocked_packages, dependency.request.package) != context.unlocked_packages.end();
        const LockedPackage* locked = context.lock && !unlocked ? context.lock->find(dependency.request.package) : nullptr;
        const bool provider_is_locked = locked && locked->provider == provider_info.name;
        const bool strict = context.lock_mode == LockMode::locked || context.lock_mode == LockMode::frozen;
        if (locked && locked->provider && !provider_is_locked && strict) {
            return std::unexpected(error_at(
                dependency.location,
                "Kaixa.lock routes `"
                    + dependency.request.package
                    + "` through provider `"
                    + *locked->provider
                    + "`, not `"
                    + provider_info.name
                    + "`"
            ));
        }

        ProviderCandidateSearch search;
        for (std::size_t index = 0; index < candidates->size(); ++index) {
            const PackageCandidate& candidate = (*candidates)[index];
            auto compatible = provider_candidate_is_compatible(candidate, dependency.request, provider_info.name, dependency.location);
            if (!compatible)
                return std::unexpected(compatible.error());

            if (!*compatible)
                continue;

            auto considered = consider_provider_candidate(
                search,
                *candidates,
                index,
                provider_is_locked ? locked : nullptr,
                provider_info.name,
                context.context_directory,
                dependency.location
            );
            if (!considered)
                return std::unexpected(considered.error());
        }

        if (search.locked)
            return std::move((*candidates)[*search.locked]);

        if (provider_is_locked && strict) {
            return std::unexpected(error_at(
                dependency.location,
                "provider `"
                    + provider_info.name
                    + "` no longer offers the candidate pinned for `"
                    + dependency.request.package
                    + "` in Kaixa.lock"
            ));
        }

        if (!search.best) {
            return std::unexpected(error_at(
                dependency.location,
                "provider `" + provider_info.name + "` has no compatible version of `" + dependency.request.package + "`"
            ));
        }
        if (search.ambiguous) {
            const std::string version = (*candidates)[*search.best].version ? (*candidates)[*search.best].version->text : "unversioned";
            return std::unexpected(error_at(
                dependency.location,
                "provider `"
                    + provider_info.name
                    + "` returned multiple candidates for `"
                    + dependency.request.package
                    + "` version `"
                    + version
                    + "`"
            ));
        }
        return std::move((*candidates)[*search.best]);
    }
}
