#include <kaixa/source/cache.hpp>

#include <kaixa/foundation/filesystem.hpp>
#include <kaixa/foundation/hash.hpp>
#include <kaixa/foundation/process.hpp>

#include <chrono>
#include <thread>

namespace kaixa {
    namespace {
        constexpr std::string_view metadata_name = ".kaixa-source";

        std::optional<SourceTree> read_entry(const std::filesystem::path& entry) {
            std::error_code failure;
            const std::filesystem::path tree = entry / "tree";
            const std::filesystem::path metadata = entry / metadata_name;
            if (!std::filesystem::is_directory(tree, failure) || failure || !std::filesystem::is_regular_file(metadata, failure) || failure)
                return std::nullopt;

            auto contents = read_file(metadata);
            if (!contents)
                return std::nullopt;

            const std::size_t separator = contents->find('\n');
            const std::string identity = contents->substr(0, separator);
            const std::string integrity = separator == std::string::npos ? std::string{} : contents->substr(separator + 1);
            return SourceTree{tree,
                identity.empty() ? std::nullopt : std::optional{identity},
                integrity.empty() ? std::nullopt : std::optional{integrity},
                true};
        }

        Result<void> remove_tree(const std::filesystem::path& path) {
            std::error_code failure;
            std::filesystem::remove_all(path, failure);
            if (failure)
                return std::unexpected(error("cannot remove incomplete source cache entry `" + path.string() + "`: " + failure.message()));

            return {};
        }

        bool stale_lock(const std::filesystem::path& lock) {
            std::error_code failure;
            const auto written = std::filesystem::last_write_time(lock, failure);
            if (failure)
                return false;

            return std::filesystem::file_time_type::clock::now() - written > std::chrono::minutes(10);
        }

        std::string safe_component(const std::string_view value) {
            std::string result;
            result.reserve(value.size());
            for (const char character: value) {
                const bool safe = (character >= 'a' && character <= 'z')
                    || (character >= 'A' && character <= 'Z')
                    || (character >= '0' && character <= '9')
                    || character == '-'
                    || character == '_';
                result += safe ? character : '_';
            }
            return result.empty() ? "source" : result;
        }
    }

    std::filesystem::path default_source_cache() {
#ifdef _WIN32
        if (const auto directory = environment_variable("LOCALAPPDATA"))
            return std::filesystem::path(*directory) / "Kaixa" / "cache" / "sources";
#else
        if (const auto directory = environment_variable("XDG_CACHE_HOME"))
            return std::filesystem::path(*directory) / "kaixa" / "sources";

        if (const auto directory = environment_variable("HOME"))
            return std::filesystem::path(*directory) / ".cache" / "kaixa" / "sources";
#endif
        std::error_code failure;
        std::filesystem::path temporary = std::filesystem::temp_directory_path(failure);
        if (failure)
            temporary = ".kaixa";

        return temporary / "kaixa-cache" / "sources";
    }

    Result<SourceTree> materialize_source_cache(
        const SourceContext& context,
        const std::string_view driver,
        const std::string_view key,
        const SourceCachePopulate& populate
    ) {
        const std::filesystem::path root = context.cache / safe_component(driver);
        const std::string cache_key = sha256(key);
        const std::filesystem::path entry = root / cache_key;
        if (!context.refresh) {
            if (auto available = read_entry(entry)) {
                if (context.expected_identity && available->identity != context.expected_identity)
                    return std::unexpected(error("cached source identity does not match Kaixa.lock"));

                if (context.expected_integrity && available->integrity != context.expected_integrity)
                    return std::unexpected(error("cached source integrity does not match Kaixa.lock"));

                return std::move(*available);
            }
        }

        if (context.offline) {
            return std::unexpected(error("source is not present in the cache and frozen mode forbids network access")
                    .add_note("cache entry: " + entry.string()));
        }

        std::error_code failure;
        std::filesystem::create_directories(root, failure);
        if (failure)
            return std::unexpected(error("cannot create source cache `" + root.string() + "`: " + failure.message()));

        const std::filesystem::path lock = root / (cache_key + ".lock");
        bool owns_lock = false;
        for (std::size_t attempt = 0; attempt < 1200; ++attempt) {
            failure.clear();
            owns_lock = std::filesystem::create_directory(lock, failure);
            if (owns_lock)
                break;

            if (failure)
                return std::unexpected(error("cannot acquire source cache lock `" + lock.string() + "`: " + failure.message()));

            if (!context.refresh) {
                if (auto available = read_entry(entry))
                    return std::move(*available);
            }

            if (stale_lock(lock)) {
                auto removed = remove_tree(lock);
                if (!removed)
                    return std::unexpected(removed.error());
            } else {
                if (attempt == 0 && context.progress)
                    context.progress("waiting for another Kaixa process to materialize the source");

                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        if (!owns_lock)
            return std::unexpected(error("timed out waiting for source cache lock `" + lock.string() + "`"));

        struct LockGuard {
            std::filesystem::path path;
            ~LockGuard() {
                std::error_code ignored;
                std::filesystem::remove_all(path, ignored);
            }
        } guard{lock};

        if (!context.refresh) {
            if (auto available = read_entry(entry))
                return std::move(*available);
        }

        auto removed = remove_tree(entry);
        if (!removed)
            return std::unexpected(removed.error());

        const std::filesystem::path staging = root / (cache_key + ".partial");
        removed = remove_tree(staging);
        if (!removed)
            return std::unexpected(removed.error());

        std::filesystem::create_directories(staging / "tree", failure);
        if (failure)
            return std::unexpected(error("cannot create source staging directory: " + failure.message()));

        struct StagingGuard {
            std::filesystem::path path;
            bool committed = false;
            ~StagingGuard() {
                if (committed)
                    return;

                std::error_code ignored;
                std::filesystem::remove_all(path, ignored);
            }
        } staging_guard{staging};

        auto populated = populate(staging / "tree");
        if (!populated)
            return std::unexpected(populated.error());

        if (context.expected_identity && populated->identity != context.expected_identity) {
            return std::unexpected(error("materialized source identity does not match Kaixa.lock")
                    .add_note("expected: " + *context.expected_identity)
                    .add_note("actual: " + populated->identity.value_or("<missing>")));
        }
        if (context.expected_integrity && populated->integrity != context.expected_integrity) {
            return std::unexpected(error("materialized source integrity does not match Kaixa.lock")
                    .add_note("expected: " + *context.expected_integrity)
                    .add_note("actual: " + populated->integrity.value_or("<missing>")));
        }

        const std::string identity = populated->identity.value_or(std::string{});
        const std::string integrity = populated->integrity.value_or(std::string{});
        if (identity.contains('\n') || integrity.contains('\n'))
            return std::unexpected(error("source identity and integrity cannot contain newlines"));

        auto metadata = write_file(staging / metadata_name, identity + '\n' + integrity);
        if (!metadata)
            return std::unexpected(metadata.error());

        std::filesystem::rename(staging, entry, failure);
        if (failure)
            return std::unexpected(error("cannot commit source cache entry `" + entry.string() + "`: " + failure.message()));

        staging_guard.committed = true;
        populated->directory = entry / "tree";
        populated->cache_hit = false;
        return std::move(*populated);
    }
}
