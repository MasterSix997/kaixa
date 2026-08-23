#include <kaixa/package/manager.hpp>

#include <kaixa/config/parser.hpp>
#include <kaixa/foundation/filesystem.hpp>
#include <kaixa/foundation/hash.hpp>
#include <kaixa/foundation/process.hpp>

#include <algorithm>
#include <chrono>
#include <regex>
#include <thread>

namespace kaixa {
    namespace {
        std::string toml_string(const std::string_view value) {
            std::string result{"\""};
            for (const char character: value) {
                switch (character) {
                case '\\': result += "\\\\"; break;
                case '"': result += "\\\""; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                default: result += character; break;
                }
            }
            result += '"';
            return result;
        }

        std::string dependency_line(
            const std::string_view package,
            const std::string_view requirement,
            const std::optional<std::string>& provider
        ) {
            if (!provider)
                return std::string(package) + " = " + toml_string(requirement);

            return std::string(package) + " = { version = " + toml_string(requirement) + ", from = " + toml_string(*provider) + " }";
        }

        struct Section {
            std::size_t header = std::string::npos;
            std::size_t content = std::string::npos;
            std::size_t end = std::string::npos;
        };

        Section dependencies_section(const std::string& contents) {
            Section section;
            std::size_t line = 0;
            while (line < contents.size()) {
                const std::size_t next = contents.find('\n', line);
                const std::size_t length = (next == std::string::npos ? contents.size() : next) - line;
                std::string_view text(contents.data() + line, length);
                if (!text.empty() && text.back() == '\r')
                    text.remove_suffix(1);

                if (section.header == std::string::npos && text == "[dependencies]") {
                    section.header = line;
                    section.content = next == std::string::npos ? contents.size() : next + 1;
                } else if (section.header != std::string::npos && !text.empty() && text.front() == '[') {
                    section.end = line;
                    break;
                }
                if (next == std::string::npos)
                    break;

                line = next + 1;
            }
            if (section.header != std::string::npos && section.end == std::string::npos)
                section.end = contents.size();

            return section;
        }

        std::optional<std::pair<std::size_t, std::size_t>> dependency_range(
            const std::string& contents,
            const Section& section,
            const std::string_view package
        ) {
            if (section.header == std::string::npos)
                return std::nullopt;

            const std::regex declaration("^[ \\t]*" + std::string(package) + "[ \\t]*=");
            std::size_t line = section.content;
            while (line < section.end) {
                const std::size_t next = contents.find('\n', line);
                const std::size_t physical_end = next == std::string::npos ? contents.size() : next + 1;
                const std::string text = contents.substr(line, (next == std::string::npos ? contents.size() : next) - line);
                if (std::regex_search(text, declaration))
                    return std::pair{line, physical_end};

                if (next == std::string::npos)
                    break;

                line = next + 1;
            }
            return std::nullopt;
        }

        Result<void> validate_edit(const ManifestEdit& edit) {
            auto parsed = parse_string(edit.after, edit.path.string());
            if (!parsed)
                return std::unexpected(parsed.error());

            auto manifest = parse_manifest_document(*parsed);
            if (!manifest)
                return std::unexpected(manifest.error());

            return {};
        }

        Result<void> copy_package_tree(
            const std::filesystem::path& source,
            const std::filesystem::path& destination,
            const std::filesystem::path& registry
        ) {
            std::error_code failure;
            std::filesystem::create_directories(destination, failure);
            if (failure)
                return std::unexpected(error("cannot create package staging directory: " + failure.message()));

            const std::filesystem::path normalized_registry = std::filesystem::absolute(registry, failure).lexically_normal();
            failure.clear();
            for (
                std::filesystem::recursive_directory_iterator iterator(source, failure), end; iterator != end; iterator.increment(failure)
            ) {
                if (failure)
                    return std::unexpected(error("cannot enumerate package contents: " + failure.message()));

                const std::filesystem::path relative = iterator->path().lexically_relative(source);
                if (relative.empty())
                    continue;

                const std::filesystem::path first = *relative.begin();
                if (first == ".git" || first == ".kaixa") {
                    if (iterator->is_directory())
                        iterator.disable_recursion_pending();

                    continue;
                }
                const std::filesystem::path absolute = std::filesystem::absolute(iterator->path(), failure).lexically_normal();
                failure.clear();
                if (!normalized_registry.empty()
                    && (absolute == normalized_registry
                        || absolute.string().starts_with(
                            normalized_registry.string() + std::string(1, std::filesystem::path::preferred_separator)
                        ))) {
                    if (iterator->is_directory())
                        iterator.disable_recursion_pending();

                    continue;
                }
                if (iterator->is_symlink(failure))
                    return std::unexpected(error("package publication rejects symlink `" + relative.generic_string() + "`"));

                const std::filesystem::path target = destination / relative;
                if (iterator->is_directory(failure)) {
                    std::filesystem::create_directories(target, failure);
                } else if (iterator->is_regular_file(failure)) {
                    std::filesystem::create_directories(target.parent_path(), failure);
                    if (!failure)
                        std::filesystem::copy_file(iterator->path(), target, std::filesystem::copy_options::overwrite_existing, failure);
                }
                if (failure)
                    return std::unexpected(error("cannot stage package entry `" + relative.generic_string() + "`: " + failure.message()));
            }
            return {};
        }

        Result<void> create_archive(const std::filesystem::path& directory, const std::filesystem::path& archive) {
            auto process = run_process({{"cmake", "-E", "tar", "czf", archive.string(), "--format=gnutar", "."}, directory, {}, true});
            if (!process)
                return std::unexpected(process.error());

            if (!process->succeeded())
                return std::unexpected(error("cannot create package archive").add_note(process->output));

            return {};
        }

        Result<void> append_index_entry(
            const std::filesystem::path& index,
            const Manifest& manifest,
            const std::filesystem::path& archive,
            const std::string& integrity,
            const bool prebuilt
        ) {
            std::string contents;
            std::error_code failure;
            if (std::filesystem::is_regular_file(index, failure)) {
                auto existing = read_file(index);
                if (!existing)
                    return std::unexpected(existing.error());

                contents = std::move(*existing);
                auto parsed = parse_string(contents, index.string());
                if (!parsed)
                    return std::unexpected(parsed.error());

                const Value* packages = parsed->find("package");
                if (packages && packages->as_array()) {
                    for (const Value& value: *packages->as_array()) {
                        const Value* name = value.find("name");
                        const Value* version = value.find("version");
                        if (name
                            && name->as_string()
                            && version
                            && version->as_string()
                            && *name->as_string() == manifest.name
                            && *version->as_string() == manifest.version->text) {
                            return std::unexpected(
                                error("registry already contains `" + manifest.name + "` version `" + manifest.version->text + "`")
                            );
                        }
                    }
                }
            }
            if (!contents.empty() && contents.back() != '\n')
                contents += '\n';

            if (!contents.empty())
                contents += '\n';

            contents += "[[package]]\n";
            contents += "name = " + toml_string(manifest.name) + "\n";
            contents += "version = " + toml_string(manifest.version->text) + "\n";
            contents += "authority = \"local\"\n";
            contents += "resolver = " + toml_string(manifest.resolver) + "\n";
            const std::string relative = archive.lexically_relative(index.parent_path()).generic_string();
            if (prebuilt) {
                contents += "artifact = { archive = { url = " + toml_string(relative) + ", sha256 = " + toml_string(integrity) + " } }\n";
                contents += "descriptor = { source = { driver = \"path\", path = \".\" } }\n";
            } else {
                contents += "source = { archive = { url = " + toml_string(relative) + ", sha256 = " + toml_string(integrity) + " } }\n";
            }
            return write_file_atomic(index, contents);
        }
    }

    Result<ManifestEdit> add_manifest_dependency(
        const std::filesystem::path& manifest,
        const std::string_view package,
        const std::string_view requirement,
        const std::optional<std::string>& provider
    ) {
        if (!is_valid_package_name(package))
            return std::unexpected(error("`" + std::string(package) + "` is not a valid package name"));

        auto parsed_requirement = parse_version_requirement(requirement);
        if (!parsed_requirement)
            return std::unexpected(parsed_requirement.error());

        auto contents = read_file(manifest);
        if (!contents)
            return std::unexpected(contents.error());

        ManifestEdit edit{manifest, *contents, *contents};
        const std::string newline = contents->contains("\r\n") ? "\r\n" : "\n";
        const Section section = dependencies_section(edit.after);
        const std::string line = dependency_line(package, requirement, provider) + newline;
        if (const auto existing = dependency_range(edit.after, section, package)) {
            edit.after.replace(existing->first, existing->second - existing->first, line);
        } else if (section.header != std::string::npos) {
            edit.after.insert(section.end, line);
        } else {
            if (!edit.after.empty() && !edit.after.ends_with('\n'))
                edit.after += newline;

            if (!edit.after.empty())
                edit.after += newline;

            edit.after += "[dependencies]" + newline + line;
        }
        auto valid = validate_edit(edit);
        if (!valid)
            return std::unexpected(valid.error());

        return edit;
    }

    Result<ManifestEdit> remove_manifest_dependency(const std::filesystem::path& manifest, const std::string_view package) {
        auto contents = read_file(manifest);
        if (!contents)
            return std::unexpected(contents.error());

        ManifestEdit edit{manifest, *contents, *contents};
        const Section section = dependencies_section(edit.after);
        const auto existing = dependency_range(edit.after, section, package);
        if (!existing)
            return std::unexpected(error("manifest does not declare dependency `" + std::string(package) + "`"));

        edit.after.erase(existing->first, existing->second - existing->first);
        auto valid = validate_edit(edit);
        if (!valid)
            return std::unexpected(valid.error());

        return edit;
    }

    Result<void> apply_manifest_edit(const ManifestEdit& edit) {
        if (!edit.changed())
            return {};

        return write_file_atomic(edit.path, edit.after);
    }

    Result<PublishResult> publish_remote_package(
        const PublishRequest& request,
        const Manifest& manifest,
        const std::filesystem::path& staging,
        const std::filesystem::path& archive,
        const std::string& integrity
    ) {
        std::string endpoint = *request.endpoint;
        while (endpoint.ends_with('/'))
            endpoint.pop_back();

        const std::size_t scheme = endpoint.find("://");
        const std::size_t authority_end = scheme == std::string::npos ? 0 : endpoint.find('/', scheme + 3);
        if (scheme != std::string::npos
            && endpoint.find('@', scheme + 3) < (authority_end == std::string::npos ? endpoint.size() : authority_end)) {
            return std::unexpected(error("registry URL cannot contain embedded credentials; use `--token-env`"));
        }

        std::string metadata;
        metadata += "name = " + toml_string(manifest.name) + "\n";
        metadata += "version = " + toml_string(manifest.version->text) + "\n";
        metadata += "resolver = " + toml_string(manifest.resolver) + "\n";
        metadata += "sha256 = " + toml_string(integrity) + "\n";
        metadata += std::string("kind = ") + toml_string(request.prebuilt ? "prebuilt" : "source") + "\n";
        auto metadata_written = write_file(staging / "metadata.toml", metadata);
        if (!metadata_written)
            return std::unexpected(metadata_written.error());

        std::vector<std::string> arguments{
            "curl",
            "--fail",
            "--silent",
            "--show-error",
            "--request",
            "POST",
            "--form",
            "metadata=@" + (staging / "metadata.toml").string() + ";type=application/toml",
            "--form",
            "package=@" + archive.string() + ";type=application/gzip",
        };
        std::optional<std::filesystem::path> credential_file;
        if (request.token_environment) {
            const auto token = environment_variable(*request.token_environment);
            if (!token)
                return std::unexpected(error("credential environment variable `" + *request.token_environment + "` is not set"));

            if (token->contains('\n') || token->contains('\r'))
                return std::unexpected(error("credential environment variable contains a newline"));

            std::string escaped;
            for (const char character: *token) {
                if (character == '\\' || character == '"')
                    escaped += '\\';

                escaped += character;
            }
            credential_file = staging / ".curl-config";
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
        const std::string publish_url = endpoint + "/api/v1/packages";
        arguments.push_back(publish_url);
        auto uploaded = run_process({std::move(arguments), request.package, {}, true});
        if (credential_file) {
            std::error_code ignored;
            std::filesystem::remove(*credential_file, ignored);
        }
        if (!uploaded)
            return std::unexpected(uploaded.error());

        if (!uploaded->succeeded()) {
            return std::unexpected(
                error("registry rejected publication of `" + manifest.name + "` with exit code " + std::to_string(uploaded->exit_code))
                    .add_note(uploaded->output)
            );
        }
        return PublishResult{manifest.name, *manifest.version, publish_url, integrity, request.prebuilt.has_value()};
    }

    Result<PublishResult> publish_package(const PublishRequest& request) {
        auto manifest = parse_manifest_file(request.package / "Kaixa.toml");
        if (!manifest)
            return std::unexpected(manifest.error());

        if (!manifest->version)
            return std::unexpected(error_at(manifest->location, "published packages must declare a version"));

        for (const DependencyBinding& dependency: manifest->dependencies) {
            const bool local_path = dependency.selection.path
                || dependency.selection.source && dependency.selection.source->driver == "path";
            if (local_path) {
                return std::unexpected(
                    error_at(dependency.location, "published dependency `" + dependency.request.package + "` cannot use a local path")
                        .add_note("publish the dependency and use a version requirement plus provider routing")
                );
            }
            if (!dependency.request.version) {
                return std::unexpected(error_at(
                    dependency.location,
                    "published dependency `" + dependency.request.package + "` must declare a version requirement"
                ));
            }
        }

        const std::filesystem::path contents = request.prebuilt.value_or(request.package);
        std::error_code failure;
        if (!std::filesystem::is_directory(contents, failure) || failure)
            return std::unexpected(error("publication input is not a directory: " + contents.string()));

        const std::filesystem::path relative = std::filesystem::path("packages") / manifest->name / manifest->version->text;
        if (request.dry_run) {
            const std::string destination = request.endpoint ? *request.endpoint + "/api/v1/packages"
                                                             : (request.registry / relative / "<sha256>.tar.gz").string();
            return PublishResult{manifest->name, *manifest->version, destination, "<sha256>", request.prebuilt.has_value()};
        }

        const std::string staging_key = sha256(
            manifest->name + manifest->version->text + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
        );
        const std::filesystem::path staging = std::filesystem::temp_directory_path() / "kaixa-publish" / staging_key;
        std::filesystem::remove_all(staging, failure);
        failure.clear();
        struct PublicationStagingGuard {
            std::filesystem::path path;
            ~PublicationStagingGuard() {
                std::error_code ignored;
                std::filesystem::remove_all(path, ignored);
            }
        } staging_guard{staging};

        auto copied = copy_package_tree(contents, staging / "contents", request.registry);
        if (!copied)
            return std::unexpected(copied.error());

        std::filesystem::create_directories(staging, failure);
        const std::filesystem::path temporary_archive = staging / "package.tar.gz";
        auto archived = create_archive(staging / "contents", temporary_archive);
        if (!archived)
            return std::unexpected(archived.error());

        auto integrity = sha256_file(temporary_archive);
        if (!integrity)
            return std::unexpected(integrity.error());

        if (request.endpoint)
            return publish_remote_package(request, *manifest, staging, temporary_archive, *integrity);

        const std::filesystem::path archive = request.registry / relative / (*integrity + ".tar.gz");
        std::filesystem::create_directories(archive.parent_path(), failure);
        if (failure)
            return std::unexpected(error("cannot create registry package directory: " + failure.message()));

        const std::filesystem::path index_lock = request.registry / ".index.lock";
        bool owns_lock = false;
        for (std::size_t attempt = 0; attempt < 600; ++attempt) {
            failure.clear();
            owns_lock = std::filesystem::create_directory(index_lock, failure);
            if (owns_lock)
                break;

            if (failure)
                return std::unexpected(error("cannot acquire registry index lock: " + failure.message()));

            const auto written = std::filesystem::last_write_time(index_lock, failure);
            if (!failure && std::filesystem::file_time_type::clock::now() - written > std::chrono::minutes(10)) {
                std::filesystem::remove_all(index_lock, failure);
                if (failure)
                    return std::unexpected(error("cannot remove stale registry index lock: " + failure.message()));
            } else {
                failure.clear();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        if (!owns_lock)
            return std::unexpected(error("timed out waiting for registry index lock `" + index_lock.string() + "`"));

        struct IndexLockGuard {
            std::filesystem::path path;
            ~IndexLockGuard() {
                std::error_code ignored;
                std::filesystem::remove_all(path, ignored);
            }
        } index_guard{index_lock};

        const bool archive_exists = std::filesystem::is_regular_file(archive, failure) && !failure;
        if (!archive_exists) {
            std::filesystem::rename(temporary_archive, archive, failure);
            if (failure)
                return std::unexpected(error("cannot publish package archive: " + failure.message()));
        }

        auto indexed = append_index_entry(request.registry / "index.toml", *manifest, archive, *integrity, request.prebuilt.has_value());
        if (!indexed) {
            if (!archive_exists) {
                std::error_code ignored;
                std::filesystem::remove(archive, ignored);
            }
            return std::unexpected(indexed.error());
        }

        return PublishResult{manifest->name, *manifest->version, archive.string(), *integrity, request.prebuilt.has_value()};
    }
}
