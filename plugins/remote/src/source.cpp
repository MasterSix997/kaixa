#include <kaixa/plugin/remote/source.hpp>

#include <kaixa/foundation/filesystem.hpp>
#include <kaixa/foundation/hash.hpp>
#include <kaixa/foundation/process.hpp>
#include <kaixa/source/cache.hpp>

#include <algorithm>
#include <fstream>
#include <system_error>

namespace kaixa::plugin::remote {
    namespace {
        struct CommonOptions {
            std::string url;
            std::optional<std::string> sha256;
            std::optional<std::string> token_environment;
        };

        Result<CommonOptions> common_options(const SourceLocator& source, const bool checksum_required) {
            const std::vector<TableEntry>* table = source.options.as_table();
            if (!table)
                return std::unexpected(error(source.driver + " source options must be a table"));

            CommonOptions result;
            for (const TableEntry& entry: *table) {
                if (entry.key == "url") {
                    if (!entry.value.as_string())
                        return std::unexpected(error_at(entry.value.location(), "source `url` must be a string"));

                    result.url = *entry.value.as_string();
                } else if (entry.key == "sha256") {
                    if (!entry.value.as_string())
                        return std::unexpected(error_at(entry.value.location(), "source `sha256` must be a string"));

                    result.sha256 = *entry.value.as_string();
                } else if (entry.key == "token-env") {
                    if (!entry.value.as_string())
                        return std::unexpected(error_at(entry.value.location(), "source `token-env` must be a string"));

                    result.token_environment = *entry.value.as_string();
                } else if (source.driver != "git" && entry.key != "filename" && entry.key != "strip-prefix") {
                    return std::unexpected(
                        error_at(entry.value.location(), "unknown " + source.driver + " source option `" + entry.key + "`")
                    );
                }
            }
            if (result.url.empty())
                return std::unexpected(error_at(source.options.location(), source.driver + " source requires a string `url`"));

            const std::size_t scheme = result.url.find("://");
            const std::size_t authority_end = scheme == std::string::npos ? 0 : result.url.find('/', scheme + 3);
            if (scheme != std::string::npos
                && result.url.find('@', scheme + 3) < (authority_end == std::string::npos ? result.url.size() : authority_end)) {
                return std::unexpected(
                    error_at(source.options.location(), "source URL cannot contain embedded credentials; use `token-env`")
                );
            }

            if (checksum_required && !result.sha256) {
                return std::unexpected(
                    error_at(source.options.location(), source.driver + " source requires `sha256` to verify downloaded content")
                );
            }
            if (result.sha256 && (result.sha256->size() != 64 || !std::ranges::all_of(*result.sha256, [](const char character) {
                    return (character >= '0' && character <= '9')
                        || (character >= 'a' && character <= 'f')
                        || (character >= 'A' && character <= 'F');
                }))) {
                return std::unexpected(error_at(source.options.location(), "source `sha256` must contain 64 hexadecimal characters"));
            }
            if (result.sha256)
                std::ranges::transform(*result.sha256, result.sha256->begin(), [](const unsigned char character) {
                    return static_cast<char>(std::tolower(character));
                });

            return result;
        }

        Result<void> run_checked(ProcessRequest request, const std::string_view operation) {
            request.capture_output = true;
            auto result = run_process(request);
            if (!result)
                return std::unexpected(result.error());

            if (!result->succeeded()) {
                Diagnostic diagnostic = error(std::string(operation) + " failed with exit code " + std::to_string(result->exit_code));
                if (!result->output.empty())
                    diagnostic.notes.push_back(result->output);

                return std::unexpected(std::move(diagnostic));
            }
            return {};
        }

        Result<void> download(const CommonOptions& options, const std::filesystem::path& destination, const SourceContext& context) {
            if (context.progress)
                context.progress("downloading " + options.url);

            std::vector<std::string> arguments{
                "curl",
                "--fail",
                "--location",
                "--silent",
                "--show-error",
                "--max-filesize",
                "2147483648",
                "--output",
                destination.string(),
            };
            std::optional<std::filesystem::path> credential_file;
            if (options.token_environment) {
                const auto token = environment_variable(*options.token_environment);
                if (!token) {
                    return std::unexpected(error("credential environment variable `" + *options.token_environment + "` is not set"));
                }
                if (token->contains('\n') || token->contains('\r'))
                    return std::unexpected(error("credential environment variable contains a newline"));

                std::string escaped;
                escaped.reserve(token->size());
                for (const char character: *token) {
                    if (character == '\\' || character == '"')
                        escaped += '\\';

                    escaped += character;
                }
                credential_file = destination.parent_path() / ".curl-config";
                auto written = write_file(*credential_file, "header = \"Authorization: Bearer " + escaped + "\"\n");
                if (!written)
                    return std::unexpected(written.error());

#ifndef _WIN32
                std::error_code permission_failure;
                std::filesystem::permissions(
                    *credential_file,
                    std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                    std::filesystem::perm_options::replace,
                    permission_failure
                );
                if (permission_failure)
                    return std::unexpected(error("cannot protect the temporary credential file: " + permission_failure.message()));
#endif

                arguments.emplace_back("--config");
                arguments.emplace_back(credential_file->string());
            }
            arguments.push_back(options.url);
            auto downloaded = run_checked({std::move(arguments), context.requester}, "download of `" + options.url + "`");
            if (credential_file) {
                std::error_code ignored;
                std::filesystem::remove(*credential_file, ignored);
            }
            return downloaded;
        }

        Result<std::string> verify_download(const std::filesystem::path& file, const std::optional<std::string>& expected) {
            auto actual = sha256_file(file);
            if (!actual)
                return std::unexpected(actual.error());

            if (expected && *actual != *expected) {
                return std::unexpected(error("downloaded content failed SHA-256 verification")
                        .add_note("expected: " + *expected)
                        .add_note("actual:   " + *actual));
            }
            return *actual;
        }

        std::string option_string(const SourceLocator& source, const std::string_view key, const std::string_view fallback = {}) {
            const Value* value = source.options.find(key);
            return value && value->as_string() ? *value->as_string() : std::string(fallback);
        }

        std::string cache_key(const SourceLocator& source) {
            std::vector<std::pair<std::string, std::string>> fields;
            if (const std::vector<TableEntry>* table = source.options.as_table()) {
                for (const TableEntry& entry: *table) {
                    if (entry.key == "token-env")
                        continue;

                    if (entry.value.as_string())
                        fields.emplace_back(entry.key, *entry.value.as_string());
                }
            }
            std::ranges::sort(fields);
            std::string result = source.driver;
            for (const auto& [key, value]: fields) {
                result += '\n';
                result += key;
                result += '=';
                result += value;
            }

            return result;
        }

        Result<std::optional<SourceTree>> cached_only(
            const SourceLocator& source,
            SourceContext context,
            const SourceCachePopulate& populate
        ) {
            context.offline = true;
            context.refresh = false;
            auto tree = materialize_source_cache(context, source.driver, cache_key(source), populate);
            if (!tree && tree.error().message.contains("not present in the cache"))
                return std::optional<SourceTree>{};

            if (!tree)
                return std::unexpected(tree.error());

            return std::optional{std::move(*tree)};
        }

        class GitSourceDriver final : public SourceDriver {
        public:
            [[nodiscard]] SourceDriverInfo info() const override { return {"git", "clones and pins a Git source tree"}; }

            [[nodiscard]] Result<std::optional<SourceTree>> locate(
                const SourceLocator& source,
                const SourceContext& context
            ) const override {
                return cached_only(source, context, [&](const std::filesystem::path&) -> Result<SourceTree> {
                    return std::unexpected(error("offline cache population is unreachable"));
                });
            }

            [[nodiscard]] Result<std::optional<SourceTree>> materialize(
                const SourceLocator& source,
                const SourceContext& context
            ) const override {
                auto options = common_options(source, false);
                if (!options)
                    return std::unexpected(options.error());

                const std::string revision = context.expected_identity.value_or(option_string(
                    source,
                    "commit",
                    option_string(source, "rev", option_string(source, "tag", option_string(source, "branch", "HEAD")))
                ));
                std::size_t selectors = 0;
                for (const std::string_view key: {"commit", "rev", "tag", "branch"}) {
                    if (source.options.find(key))
                        ++selectors;
                }
                if (selectors > 1)
                    return std::unexpected(error_at(source.options.location(), "git source accepts only one revision selector"));

                for (const TableEntry& entry: *source.options.as_table()) {
                    if (entry.key != "url"
                        && entry.key != "commit"
                        && entry.key != "rev"
                        && entry.key != "tag"
                        && entry.key != "branch"
                        && entry.key != "token-env") {
                        return std::unexpected(error_at(entry.value.location(), "unknown git source option `" + entry.key + "`"));
                    }
                }
                auto tree = materialize_source_cache(
                    context,
                    "git",
                    cache_key(source),
                    [&](const std::filesystem::path& destination) -> Result<SourceTree> {
                        if (context.progress)
                            context.progress("cloning " + options->url);

                        std::vector<EnvironmentVariable> environment;
                        if (options->token_environment) {
                            const auto token = environment_variable(*options->token_environment);
                            if (!token) {
                                return std::unexpected(
                                    error("credential environment variable `" + *options->token_environment + "` is not set")
                                );
                            }
                            if (token->contains('\n') || token->contains('\r'))
                                return std::unexpected(error("credential environment variable contains a newline"));

                            environment.push_back({"GIT_HTTP_EXTRA_HEADER", "Authorization: Bearer " + *token});
                        }
                        auto cloned = run_checked(
                            {{"git", "clone", "--no-checkout", "--quiet", options->url, destination.string()},
                                context.requester,
                                std::move(environment)},
                            "Git clone of `" + options->url + "`"
                        );
                        if (!cloned)
                            return std::unexpected(cloned.error());

                        auto resolved = run_process(
                            {{"git", "-C", destination.string(), "rev-parse", "--verify", revision + "^{commit}"}, {}, {}, true}
                        );
                        if (!resolved)
                            return std::unexpected(resolved.error());

                        if (!resolved->succeeded()) {
                            return std::unexpected(
                                error("Git revision `" + revision + "` does not resolve to a commit").add_note(resolved->output)
                            );
                        }
                        std::string commit = resolved->output;
                        while (!commit.empty() && (commit.back() == '\n' || commit.back() == '\r'))
                            commit.pop_back();

                        auto checked_out = run_checked(
                            {{"git", "-C", destination.string(), "checkout", "--detach", "--quiet", commit}},
                            "Git checkout of `" + commit + "`"
                        );
                        if (!checked_out)
                            return std::unexpected(checked_out.error());

                        return SourceTree{destination, commit, "git:" + commit};
                    }
                );
                if (!tree)
                    return std::unexpected(tree.error());

                return std::optional{std::move(*tree)};
            }
        };

        class UrlSourceDriver final : public SourceDriver {
        public:
            [[nodiscard]] SourceDriverInfo info() const override { return {"url", "downloads a verified source file"}; }

            [[nodiscard]] Result<std::optional<SourceTree>> locate(
                const SourceLocator& source,
                const SourceContext& context
            ) const override {
                return cached_only(source, context, [&](const std::filesystem::path&) -> Result<SourceTree> {
                    return std::unexpected(error("offline cache population is unreachable"));
                });
            }

            [[nodiscard]] Result<std::optional<SourceTree>> materialize(
                const SourceLocator& source,
                const SourceContext& context
            ) const override {
                auto options = common_options(source, true);
                if (!options)
                    return std::unexpected(options.error());

                const std::string filename = option_string(source, "filename", "Kaixa.toml");
                const std::filesystem::path relative{filename};
                if (relative.empty()
                    || relative.is_absolute()
                    || relative.has_root_path()
                    || std::ranges::find(relative, "..") != relative.end()) {
                    return std::unexpected(error_at(source.options.location(), "URL source filename must stay inside the source tree"));
                }
                auto tree = materialize_source_cache(
                    context,
                    "url",
                    cache_key(source),
                    [&](const std::filesystem::path& destination) -> Result<SourceTree> {
                        const std::filesystem::path file = destination / relative;
                        std::error_code failure;
                        std::filesystem::create_directories(file.parent_path(), failure);
                        if (failure)
                            return std::unexpected(error("cannot create URL source directory: " + failure.message()));

                        auto downloaded = download(*options, file, context);
                        if (!downloaded)
                            return std::unexpected(downloaded.error());

                        auto integrity = verify_download(file, options->sha256);
                        if (!integrity)
                            return std::unexpected(integrity.error());

                        return SourceTree{destination, "sha256:" + *integrity, "sha256:" + *integrity};
                    }
                );
                if (!tree)
                    return std::unexpected(tree.error());

                return std::optional{std::move(*tree)};
            }
        };

        bool safe_archive_entry(std::string name) {
            std::ranges::replace(name, '\\', '/');
            if (name.empty() || name.front() == '/' || name.starts_with("//"))
                return false;

            const std::filesystem::path path{name};
            if (path.is_absolute() || path.has_root_name() || path.has_root_directory())
                return false;

            return std::ranges::find(path, "..") == path.end();
        }

        Result<void> extract_archive(
            const std::filesystem::path& archive,
            const std::filesystem::path& destination,
            const std::string& strip_prefix
        ) {
            auto listing = run_process({{"cmake", "-E", "tar", "tf", archive.string()}, {}, {}, true});
            if (!listing)
                return std::unexpected(listing.error());

            if (!listing->succeeded())
                return std::unexpected(error("cannot list archive `" + archive.string() + "`").add_note(listing->output));

            std::size_t begin = 0;
            std::size_t entry_count = 0;
            while (begin < listing->output.size()) {
                const std::size_t end = listing->output.find('\n', begin);
                std::string entry = listing->output.substr(begin, end == std::string::npos ? end : end - begin);
                if (!entry.empty() && entry.back() == '\r')
                    entry.pop_back();

                if (!entry.empty() && !safe_archive_entry(entry))
                    return std::unexpected(error("archive contains an unsafe path `" + entry + "`"));

                ++entry_count;
                if (entry_count > 250'000)
                    return std::unexpected(error("archive contains more than 250000 entries"));

                if (end == std::string::npos)
                    break;

                begin = end + 1;
            }

            auto verbose = run_process({{"cmake", "-E", "tar", "tvf", archive.string()}, {}, {}, true});
            if (!verbose)
                return std::unexpected(verbose.error());

            if (!verbose->succeeded())
                return std::unexpected(error("cannot inspect archive entry types").add_note(verbose->output));

            begin = 0;
            while (begin < verbose->output.size()) {
                const std::size_t end = verbose->output.find('\n', begin);
                std::string_view entry(verbose->output.data() + begin, (end == std::string::npos ? verbose->output.size() : end) - begin);
                if (!entry.empty() && (entry.front() == 'l' || entry.front() == 'h'))
                    return std::unexpected(error("archive contains a symbolic or hard link, which is not allowed"));

                if (end == std::string::npos)
                    break;

                begin = end + 1;
            }

            const std::filesystem::path extraction = strip_prefix.empty() ? destination : destination.parent_path() / "archive-root";
            std::error_code failure;
            std::filesystem::create_directories(extraction, failure);
            if (failure)
                return std::unexpected(error("cannot create archive extraction directory: " + failure.message()));

            auto extracted = run_checked({{"cmake", "-E", "tar", "xf", archive.string()}, extraction}, "archive extraction");
            if (!extracted)
                return std::unexpected(extracted.error());

            std::uintmax_t extracted_size = 0;
            entry_count = 0;
            for (
                std::filesystem::recursive_directory_iterator iterator(extraction, failure), end; iterator != end;
                iterator.increment(failure)
            ) {
                if (failure)
                    return std::unexpected(error("cannot inspect extracted archive: " + failure.message()));

                ++entry_count;
                if (entry_count > 250'000)
                    return std::unexpected(error("extracted archive contains more than 250000 entries"));

                if (iterator->is_symlink(failure))
                    return std::unexpected(error("extracted archive contains a symbolic link"));

                if (iterator->is_regular_file(failure)) {
                    extracted_size += iterator->file_size(failure);
                    if (failure)
                        return std::unexpected(error("cannot inspect extracted archive file size: " + failure.message()));

                    if (extracted_size > 8ULL * 1024ULL * 1024ULL * 1024ULL)
                        return std::unexpected(error("extracted archive exceeds the 8 GiB safety limit"));
                }
            }

            if (strip_prefix.empty())
                return {};

            const std::filesystem::path selected = extraction / strip_prefix;
            if (!std::filesystem::is_directory(selected, failure) || failure) {
                return std::unexpected(error("archive does not contain strip prefix `" + strip_prefix + "`"));
            }
            std::filesystem::copy(
                selected,
                destination,
                std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
                failure
            );
            if (failure)
                return std::unexpected(error("cannot copy stripped archive contents: " + failure.message()));

            std::filesystem::remove_all(extraction, failure);
            if (failure)
                return std::unexpected(error("cannot remove archive extraction staging directory: " + failure.message()));

            return {};
        }

        class ArchiveSourceDriver final : public SourceDriver {
        public:
            [[nodiscard]] SourceDriverInfo info() const override { return {"archive", "downloads and safely extracts a verified archive"}; }

            [[nodiscard]] Result<std::optional<SourceTree>> locate(
                const SourceLocator& source,
                const SourceContext& context
            ) const override {
                return cached_only(source, context, [&](const std::filesystem::path&) -> Result<SourceTree> {
                    return std::unexpected(error("offline cache population is unreachable"));
                });
            }

            [[nodiscard]] Result<std::optional<SourceTree>> materialize(
                const SourceLocator& source,
                const SourceContext& context
            ) const override {
                auto options = common_options(source, true);
                if (!options)
                    return std::unexpected(options.error());

                const std::string strip_prefix = option_string(source, "strip-prefix");
                if (!strip_prefix.empty() && !safe_archive_entry(strip_prefix))
                    return std::unexpected(error_at(source.options.location(), "archive strip prefix must be a safe relative path"));

                auto tree = materialize_source_cache(
                    context,
                    "archive",
                    cache_key(source),
                    [&](const std::filesystem::path& destination) -> Result<SourceTree> {
                        const std::filesystem::path archive = destination.parent_path() / "download";
                        auto downloaded = download(*options, archive, context);
                        if (!downloaded)
                            return std::unexpected(downloaded.error());

                        std::error_code failure;
                        const std::uintmax_t archive_size = std::filesystem::file_size(archive, failure);
                        if (failure)
                            return std::unexpected(error("cannot inspect downloaded archive size: " + failure.message()));

                        if (archive_size > 2ULL * 1024ULL * 1024ULL * 1024ULL)
                            return std::unexpected(error("downloaded archive exceeds the 2 GiB safety limit"));

                        auto integrity = verify_download(archive, options->sha256);
                        if (!integrity)
                            return std::unexpected(integrity.error());

                        auto extracted = extract_archive(archive, destination, strip_prefix);
                        if (!extracted)
                            return std::unexpected(extracted.error());

                        std::filesystem::remove(archive, failure);
                        if (failure)
                            return std::unexpected(error("cannot remove cached archive download: " + failure.message()));

                        return SourceTree{destination, "sha256:" + *integrity, "sha256:" + *integrity};
                    }
                );
                if (!tree)
                    return std::unexpected(tree.error());

                return std::optional{std::move(*tree)};
            }
        };
    }

    std::unique_ptr<SourceDriver> make_git_source_driver() {
        return std::make_unique<GitSourceDriver>();
    }

    std::unique_ptr<SourceDriver> make_url_source_driver() {
        return std::make_unique<UrlSourceDriver>();
    }

    std::unique_ptr<SourceDriver> make_archive_source_driver() {
        return std::make_unique<ArchiveSourceDriver>();
    }
}
