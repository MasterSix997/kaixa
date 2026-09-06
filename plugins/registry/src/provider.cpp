#include <kaixa/plugin/registry/provider.hpp>

#include <kaixa/config/parser.hpp>
#include <kaixa/config/table_reader.hpp>
#include <kaixa/foundation/filesystem.hpp>
#include <kaixa/foundation/hash.hpp>
#include <kaixa/foundation/process.hpp>
#include <kaixa/source/cache.hpp>

#include <algorithm>
#include <cctype>

namespace kaixa::plugin::registry {
    namespace {
        struct RegistryPackage {
            PackageCandidate candidate;
            PackageSummary summary;
        };

        std::string lower(std::string text) {
            std::ranges::transform(text, text.begin(), [](const unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
            return text;
        }

        Result<std::vector<std::string>> strings(const Value* value, const std::string_view name) {
            if (!value)
                return std::vector<std::string>{};

            const std::vector<Value>* array = value->as_array();
            if (!array)
                return std::unexpected(error_at(value->location(), std::string(name) + " must be an array of strings"));

            std::vector<std::string> result;
            result.reserve(array->size());
            for (const Value& entry: *array) {
                if (!entry.as_string())
                    return std::unexpected(error_at(entry.location(), std::string(name) + " entries must be strings"));

                result.push_back(*entry.as_string());
            }
            return result;
        }

        Result<std::optional<SourceLocator>> locator(const Value* value, const std::string_view name) {
            if (!value)
                return std::optional<SourceLocator>{};

            const std::vector<TableEntry>* table = value->as_table();
            if (!table || table->size() != 1 || !table->front().value.is_table()) {
                return std::unexpected(error_at(value->location(), std::string(name) + " must contain exactly one source-driver table"));
            }
            return std::optional{SourceLocator{table->front().key, table->front().value}};
        }

        std::string resolve_url(const std::string& index, const std::filesystem::path& directory, const std::string& url) {
            if (url.contains("://"))
                return url;

            if (index.contains("://")) {
                const std::size_t slash = index.rfind('/');
                return slash == std::string::npos ? url : index.substr(0, slash + 1) + url;
            }

            std::error_code failure;
            std::filesystem::path path = url;
            if (path.is_relative())
                path = directory / path;

            path = std::filesystem::absolute(path, failure).lexically_normal();
            if (failure)
                return url;

            return "file:///" + path.generic_string();
        }

        SourceLocator resolve_locator_urls(SourceLocator source, const std::string& index, const std::filesystem::path& directory) {
            const std::vector<TableEntry>* options = source.options.as_table();
            if (!options)
                return source;

            std::vector<TableEntry> normalized = *options;
            for (TableEntry& option: normalized) {
                if (option.key == "url" && option.value.as_string()) {
                    option.value = Value::string(resolve_url(index, directory, *option.value.as_string()), option.value.location());
                }
            }
            source.options = Value::table(std::move(normalized), source.options.location());
            return source;
        }

        Result<std::filesystem::path> registry_index(
            const std::string& configured,
            const std::optional<std::string>& token_environment,
            const ProviderContext& context
        ) {
            const std::size_t scheme = configured.find("://");
            const std::size_t authority_end = scheme == std::string::npos ? 0 : configured.find('/', scheme + 3);
            if (scheme != std::string::npos
                && configured.find('@', scheme + 3) < (authority_end == std::string::npos ? configured.size() : authority_end)) {
                return std::unexpected(error("registry URL cannot contain embedded credentials; use `token-env`"));
            }

            if (!configured.contains("://")) {
                std::filesystem::path path = configured;
                if (path.is_relative())
                    path = context.directory / path;

                return std::filesystem::absolute(path).lexically_normal();
            }

            SourceContext source_context{context.directory, context.cache, context.offline, !context.offline};
            auto cached = materialize_source_cache(
                source_context,
                "catalog",
                configured,
                [&](const std::filesystem::path& destination) -> Result<SourceTree> {
                    const std::filesystem::path index = destination / "index.toml";
                    std::vector<std::string> arguments{
                        "curl",
                        "--fail",
                        "--location",
                        "--silent",
                        "--show-error",
                        "--output",
                        index.string(),
                    };
                    std::optional<std::filesystem::path> credential_file;
                    if (token_environment) {
                        const auto token = environment_variable(*token_environment);
                        if (!token) {
                            return std::unexpected(error("credential environment variable `" + *token_environment + "` is not set"));
                        }
                        if (token->contains('\n') || token->contains('\r'))
                            return std::unexpected(error("credential environment variable contains a newline"));

                        std::string escaped;
                        for (const char character: *token) {
                            if (character == '\\' || character == '"')
                                escaped += '\\';

                            escaped += character;
                        }
                        credential_file = destination / ".curl-config";
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
                        if (permission_failure) {
                            return std::unexpected(error("cannot protect the temporary credential file: " + permission_failure.message()));
                        }
#endif

                        arguments.emplace_back("--config");
                        arguments.emplace_back(credential_file->string());
                    }
                    arguments.push_back(configured);
                    auto downloaded = run_process({std::move(arguments), context.directory, {}, ProcessOutputMode::capture});
                    if (credential_file) {
                        std::error_code ignored;
                        std::filesystem::remove(*credential_file, ignored);
                    }
                    if (!downloaded)
                        return std::unexpected(downloaded.error());

                    if (!downloaded->succeeded()) {
                        return std::unexpected(error("cannot download registry index `" + configured + "`").add_note(downloaded->output));
                    }
                    auto integrity = sha256_file(index);
                    if (!integrity)
                        return std::unexpected(integrity.error());

                    return SourceTree{destination, "sha256:" + *integrity, "sha256:" + *integrity};
                }
            );
            if (!cached)
                return std::unexpected(cached.error());

            return cached->directory / "index.toml";
        }

        Result<std::vector<RegistryPackage>> read_packages(
            const std::filesystem::path& path,
            const std::string& configured_index,
            const ProviderInfo& provider
        ) {
            auto document = parse_file(path);
            if (!document)
                return std::unexpected(document.error());

            const Value* packages = document->find("package");
            if (!packages)
                return std::vector<RegistryPackage>{};

            const std::vector<Value>* entries = packages->as_array();
            if (!entries)
                return std::unexpected(error_at(packages->location(), "registry `package` must be an array of tables"));

            std::vector<RegistryPackage> result;
            result.reserve(entries->size());
            for (std::size_t index = 0; index < entries->size(); ++index) {
                auto package_result = TableReader::bind((*entries)[index], "package." + std::to_string(index));
                if (!package_result)
                    return std::unexpected(package_result.error());

                TableReader package = std::move(*package_result);
                auto name = package.string("name");
                auto version_text = package.string("version");
                auto authority = package.optional_string("authority");
                auto description = package.optional_string("description");
                auto resolver = package.optional_string("resolver");
                if (!name)
                    return std::unexpected(name.error());

                if (!version_text)
                    return std::unexpected(version_text.error());

                if (!authority)
                    return std::unexpected(authority.error());

                if (!description)
                    return std::unexpected(description.error());

                if (!resolver)
                    return std::unexpected(resolver.error());

                auto version = parse_version(*version_text, package.location_of("version"));
                if (!version)
                    return std::unexpected(version.error());

                auto capabilities = strings(package.take("capabilities"), "package capabilities");
                auto tags = strings(package.take("tags"), "package tags");
                auto source = locator(package.take("source"), "package source");
                auto artifact = locator(package.take("artifact"), "package artifact");
                if (!capabilities)
                    return std::unexpected(capabilities.error());

                if (!tags)
                    return std::unexpected(tags.error());

                if (!source)
                    return std::unexpected(source.error());

                if (!artifact)
                    return std::unexpected(artifact.error());

                const Value* descriptor = package.take("descriptor");
                if (!*source && !*artifact && !descriptor) {
                    return std::unexpected(
                        error_at((*entries)[index].location(), "registry package requires `source`, `artifact`, or `descriptor`")
                    );
                }
                if (*artifact && !descriptor)
                    return std::unexpected(error_at((*entries)[index].location(), "prebuilt artifact requires a `descriptor`"));

                auto finished = package.finish();
                if (!finished)
                    return std::unexpected(finished.error());

                if (*source)
                    *source = resolve_locator_urls(std::move(**source), configured_index, path.parent_path());

                if (*artifact)
                    *artifact = resolve_locator_urls(std::move(**artifact), configured_index, path.parent_path());

                const std::string resolved_authority = authority->value_or(provider.name);
                PackageCandidate candidate{*name,
                    *version,
                    resolved_authority,
                    std::move(*source),
                    *resolver,
                    descriptor ? std::optional<Value>{*descriptor} : std::nullopt,
                    std::move(*artifact)};
                PackageSummary summary{*name,
                    *version,
                    provider.name,
                    resolved_authority,
                    description->value_or(std::string{}),
                    *resolver,
                    std::move(*capabilities),
                    std::move(*tags)};
                result.push_back({std::move(candidate), std::move(summary)});
            }
            return result;
        }

        class RegistryProvider final : public PackageProvider {
        public:
            RegistryProvider(ProviderInfo info, std::string index, std::optional<std::string> token_environment, ProviderContext context)
                : m_info(std::move(info))
                , m_index(std::move(index))
                , m_token_environment(std::move(token_environment))
                , m_context(std::move(context)) {}

            [[nodiscard]] ProviderInfo info() const override { return m_info; }

            [[nodiscard]] Result<std::vector<PackageCandidate>> candidates(const PackageRequest& request) const override {
                auto available = packages();
                if (!available)
                    return std::unexpected(available.error());

                std::vector<PackageCandidate> result;
                for (const RegistryPackage& package: **available) {
                    if (package.candidate.package == request.package)
                        result.push_back(package.candidate);
                }
                return result;
            }

            [[nodiscard]] Result<std::vector<PackageSummary>> query(const PackageQuery& query) const override {
                auto available = packages();
                if (!available)
                    return std::unexpected(available.error());

                std::vector<PackageSummary> result;
                const std::string text = lower(query.text);
                for (const RegistryPackage& package: **available) {
                    const PackageSummary& summary = package.summary;
                    if (!text.empty() && !lower(summary.name).contains(text) && !lower(summary.description).contains(text))
                        continue;

                    if (query.resolver && summary.resolver != query.resolver)
                        continue;

                    if (query.capability && std::ranges::find(summary.capabilities, *query.capability) == summary.capabilities.end())
                        continue;

                    if (query.tag && std::ranges::find(summary.tags, *query.tag) == summary.tags.end())
                        continue;

                    result.push_back(summary);
                    if (result.size() == query.limit)
                        break;
                }
                std::ranges::sort(result, [](const PackageSummary& left, const PackageSummary& right) {
                    if (left.name != right.name)
                        return left.name < right.name;

                    return compare_versions(left.version, right.version) > 0;
                });
                return result;
            }

        private:
            Result<const std::vector<RegistryPackage>*> packages() const {
                if (m_packages)
                    return &*m_packages;

                auto path = registry_index(m_index, m_token_environment, m_context);
                if (!path)
                    return std::unexpected(path.error());

                auto loaded = read_packages(*path, m_index, m_info);
                if (!loaded)
                    return std::unexpected(loaded.error());

                m_packages = std::move(*loaded);
                return &*m_packages;
            }

            ProviderInfo m_info;
            std::string m_index;
            std::optional<std::string> m_token_environment;
            ProviderContext m_context;
            mutable std::optional<std::vector<RegistryPackage>> m_packages;
        };

        class RegistryProviderDriver final : public ProviderDriver {
        public:
            [[nodiscard]] ProviderDriverInfo info() const override {
                return {"kaixa-registry", "queries a local or HTTP Kaixa package registry"};
            }

            [[nodiscard]] Result<std::unique_ptr<PackageProvider>> create(
                const ProviderDefinition& definition,
                const ProviderContext& context
            ) const override {
                auto options_result = TableReader::bind(definition.options, "providers." + definition.name);
                if (!options_result)
                    return std::unexpected(options_result.error());

                TableReader options = std::move(*options_result);
                auto index = options.string("index");
                auto token_environment = options.optional_string("token-env");
                if (!index)
                    return std::unexpected(index.error());

                if (!token_environment)
                    return std::unexpected(token_environment.error());

                auto finished = options.finish();
                if (!finished)
                    return std::unexpected(finished.error());

                ProviderInfo info{definition.name, "kaixa-registry", definition.is_default};
                std::unique_ptr<PackageProvider> provider = std::make_unique<RegistryProvider>(
                    std::move(info),
                    std::move(*index),
                    std::move(*token_environment),
                    context
                );
                return provider;
            }
        };
    }

    std::unique_ptr<ProviderDriver> make_provider_driver() {
        return std::make_unique<RegistryProviderDriver>();
    }
}
