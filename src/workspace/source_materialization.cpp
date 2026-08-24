#include "source_materialization.hpp"

#include <algorithm>
#include <system_error>

namespace kaixa::workspace_detail {
    namespace {
        bool package_is_unlocked(const SourceMaterializationContext& context, const std::string_view package) {
            return context.unlock_all || std::ranges::find(context.unlocked_packages, package) != context.unlocked_packages.end();
        }

        SourceContext source_context(
            const SourceMaterializationContext& context,
            const std::filesystem::path& requester,
            const std::string_view package
        ) {
            const bool unlocked = package_is_unlocked(context, package);
            const LockedPackage* locked = context.lock && !unlocked ? context.lock->find(package) : nullptr;
            return SourceContext{requester,
                context.cache,
                context.lock_mode == LockMode::frozen,
                unlocked && context.refresh,
                locked ? locked->source_identity : std::nullopt,
                locked ? locked->source_integrity : std::nullopt,
                context.progress};
        }
    }

    Result<std::filesystem::path> canonical_directory(const std::filesystem::path& path, const SourceLocation& location) {
        std::error_code failure;
        const bool exists = std::filesystem::exists(path, failure);
        if (failure)
            return std::unexpected(error_at(location, "cannot inspect path `" + path.string() + "`: " + failure.message()));

        if (!exists)
            return std::unexpected(error_at(location, "directory does not exist: " + path.string()));

        if (!std::filesystem::is_directory(path, failure)) {
            return std::unexpected(error_at(
                location,
                failure ? "cannot inspect path `" + path.string() + "`: " + failure.message() : "path is not a directory: " + path.string()
            ));
        }

        std::filesystem::path canonical = std::filesystem::canonical(path, failure);
        if (failure)
            return std::unexpected(error_at(location, "cannot canonicalize directory `" + path.string() + "`: " + failure.message()));

        return canonical;
    }

    Result<SourceTree> materialize_source(
        const SourceMaterializationContext& context,
        const SourceLocator& source,
        const std::filesystem::path& requester,
        const std::string_view package,
        const SourceLocation& location,
        const MaterializationKind kind
    ) {
        SourceDriver* driver = context.extensions ? context.extensions->find_source_driver(source.driver) : nullptr;
        if (!driver) {
            const std::string prefix = kind == MaterializationKind::prebuilt_artifact ? "artifact source driver" : "source driver";
            return std::unexpected(error_at(location, prefix + " `" + source.driver + "` is not installed"));
        }

        auto materialized = driver->materialize(source, source_context(context, requester, package));
        if (!materialized)
            return std::unexpected(materialized.error());

        if (!*materialized) {
            if (kind == MaterializationKind::prebuilt_artifact)
                return std::unexpected(error_at(location, "prebuilt artifact is not available"));

            return std::unexpected(error_at(location, "source for package `" + std::string(package) + "` is not available locally")
                    .add_note(
                        context.lock_mode == LockMode::frozen ? "frozen mode does not access the network"
                                                              : "check the locator and source-driver configuration"
                    ));
        }
        if (!(**materialized).directory.is_absolute()) {
            return std::unexpected(error_at(location, "source driver `" + source.driver + "` returned a relative directory"));
        }

        auto directory = canonical_directory((**materialized).directory, location);
        if (!directory)
            return std::unexpected(directory.error());

        SourceTree result = std::move(**materialized);
        result.directory = std::move(*directory);
        return result;
    }
}
