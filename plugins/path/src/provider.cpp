#include <kaixa/plugin/path/provider.hpp>

#include <kaixa/config/table_reader.hpp>
#include <kaixa/workspace/package_index.hpp>

#include <algorithm>
#include <system_error>
#include <utility>

namespace kaixa::plugin::path {
    namespace {
        SourceLocator source_locator(const std::filesystem::path& root, const SourceLocation& location) {
            return {"path", Value::table({{"path", Value::string(root.generic_string(), location)}}, location)};
        }

        Result<std::optional<SourceLocator>> read_locator(const Value* value, const std::string_view name) {
            if (!value)
                return std::optional<SourceLocator>{};

            const std::vector<TableEntry>* entries = value->as_table();
            if (!entries) {
                return std::unexpected(error_at(value->location(), std::string(name) + " must contain exactly one source-driver table"));
            }
            const auto driver = std::ranges::find(*entries, std::string_view{"driver"}, &TableEntry::key);
            if (driver != entries->end()) {
                const std::string* driver_name = driver->value.as_string();
                if (!driver_name || driver_name->empty())
                    return std::unexpected(error_at(driver->value.location(), std::string(name) + " driver must be a string"));

                std::vector<TableEntry> options;
                for (const TableEntry& entry: *entries) {
                    if (entry.key != "driver")
                        options.push_back(entry);
                }
                return std::optional{SourceLocator{*driver_name, Value::table(std::move(options), value->location())}};
            }
            if (entries->size() != 1 || !entries->front().value.is_table()) {
                return std::unexpected(error_at(value->location(), std::string(name) + " must contain exactly one source-driver table"));
            }
            return std::optional{SourceLocator{entries->front().key, entries->front().value}};
        }

        class PathProvider final : public PackageProvider {
        public:
            PathProvider(ProviderInfo info, std::filesystem::path root, SourceLocation location)
                : m_info(std::move(info))
                , m_root(std::move(root))
                , m_location(std::move(location)) {}

            [[nodiscard]] ProviderInfo info() const override { return m_info; }

            [[nodiscard]] Result<std::vector<PackageCandidate>> candidates(const PackageRequest& request) const override {
                const std::filesystem::path manifest_path = m_root / "Kaixa.toml";
                auto document = parse_manifest_document_file(manifest_path);
                if (!document)
                    return std::unexpected(document.error());

                if (document->package && document->package->name == request.package)
                    return candidate(*document->package);

                if (!document->package_set)
                    return std::vector<PackageCandidate>{};

                PackageIndex packages;
                auto included = packages.include(manifest_path);
                if (!included)
                    return std::unexpected(included.error());

                const LocalPackageCandidate* package = packages.find_in_set(manifest_path, request.package);
                if (!package)
                    return std::vector<PackageCandidate>{};

                if (!package->version) {
                    return std::unexpected(error_at(
                        package->location,
                        "package `" + package->name + "` exposed by provider `" + m_info.name + "` does not declare a version"
                    ));
                }

                return std::vector{PackageCandidate{package->name, *package->version, m_info.name, source_locator(m_root, m_location)}};
            }

        private:
            Result<std::vector<PackageCandidate>> candidate(const Manifest& package) const {
                if (!package.version) {
                    return std::unexpected(error_at(
                        package.location,
                        "package `" + package.name + "` exposed by provider `" + m_info.name + "` does not declare a version"
                    ));
                }

                return std::vector{PackageCandidate{package.name, *package.version, m_info.name, source_locator(m_root, m_location)}};
            }

            ProviderInfo m_info;
            std::filesystem::path m_root;
            SourceLocation m_location;
        };

        class PathProviderDriver final : public ProviderDriver {
        public:
            [[nodiscard]] ProviderDriverInfo info() const override { return {"path", "exposes packages from a local source tree"}; }

            [[nodiscard]] Result<std::unique_ptr<PackageProvider>> create(
                const ProviderDefinition& definition,
                const ProviderContext& context
            ) const override {
                auto options_result = TableReader::bind(definition.options, "provider." + definition.name);
                if (!options_result)
                    return std::unexpected(options_result.error());

                TableReader options = std::move(*options_result);
                auto configured_path = options.string("path");
                if (!configured_path)
                    return std::unexpected(configured_path.error());

                if (configured_path->empty())
                    return std::unexpected(error_at(options.location_of("path"), "provider path cannot be empty"));

                auto finished = options.finish();
                if (!finished)
                    return std::unexpected(finished.error());

                std::filesystem::path root = *configured_path;
                if (root.is_relative())
                    root = context.directory / root;

                std::error_code failure;
                root = std::filesystem::absolute(root, failure).lexically_normal();
                if (failure) {
                    return std::unexpected(error_at(
                        options.location_of("path"),
                        "cannot resolve provider path `" + *configured_path + "`: " + failure.message()
                    ));
                }

                ProviderInfo provider_info{definition.name, "path", definition.is_default};
                std::unique_ptr<PackageProvider> provider = std::make_unique<PathProvider>(
                    std::move(provider_info),
                    std::move(root),
                    definition.location
                );
                return provider;
            }
        };

        class DescriptiveProvider final : public PackageProvider {
        public:
            DescriptiveProvider(ProviderInfo info, std::vector<PackageCandidate> candidates)
                : m_info(std::move(info))
                , m_candidates(std::move(candidates)) {}

            [[nodiscard]] ProviderInfo info() const override { return m_info; }

            [[nodiscard]] Result<std::vector<PackageCandidate>> candidates(const PackageRequest& request) const override {
                std::vector<PackageCandidate> result;
                for (const PackageCandidate& candidate: m_candidates) {
                    if (candidate.package == request.package)
                        result.push_back(candidate);
                }
                return result;
            }

        private:
            ProviderInfo m_info;
            std::vector<PackageCandidate> m_candidates;
        };

        class DescriptiveProviderDriver final : public ProviderDriver {
        public:
            DescriptiveProviderDriver(std::string name, std::string description, const bool accepts_packages)
                : m_name(std::move(name))
                , m_description(std::move(description))
                , m_accepts_packages(accepts_packages) {}

            [[nodiscard]] ProviderDriverInfo info() const override { return {m_name, m_description}; }

            [[nodiscard]] Result<std::unique_ptr<PackageProvider>> create(
                const ProviderDefinition& definition,
                const ProviderContext&
            ) const override {
                auto options_result = TableReader::bind(definition.options, "provider." + definition.name);
                if (!options_result)
                    return std::unexpected(options_result.error());

                TableReader options = std::move(*options_result);

                std::vector<PackageCandidate> candidates;
                const Value* packages = options.take("package");
                if (packages) {
                    if (!m_accepts_packages) {
                        return std::unexpected(
                            error_at(packages->location(), "provider driver `" + m_name + "` does not accept package descriptors")
                        );
                    }
                    const std::vector<Value>* entries = packages->as_array();
                    if (!entries) {
                        return std::unexpected(error_at(packages->location(), "provider packages must be an array of tables"));
                    }
                    for (std::size_t index = 0; index < entries->size(); ++index) {
                        auto package_result = TableReader::bind(
                            (*entries)[index],
                            "provider." + definition.name + ".package." + std::to_string(index)
                        );
                        if (!package_result)
                            return std::unexpected(package_result.error());

                        TableReader package = std::move(*package_result);

                        auto name = package.string("name");
                        if (!name)
                            return std::unexpected(name.error());

                        if (!is_valid_package_name(*name)) {
                            return std::unexpected(error_at(package.location_of("name"), "`" + *name + "` is not a valid package name"));
                        }

                        std::optional<Version> version;
                        auto version_text = package.optional_string("version");
                        if (!version_text)
                            return std::unexpected(version_text.error());

                        if (*version_text) {
                            auto parsed = parse_version(**version_text, package.location_of("version"));
                            if (!parsed)
                                return std::unexpected(parsed.error());

                            version = std::move(*parsed);
                        }

                        std::optional<std::string> resolver;
                        auto consumer_result = package.optional_table("consumer");
                        if (!consumer_result)
                            return std::unexpected(consumer_result.error());

                        const bool has_consumer = consumer_result->has_value();

                        if (*consumer_result) {
                            TableReader consumer = std::move(**consumer_result);
                            auto selected = consumer.string("resolver");
                            if (!selected)
                                return std::unexpected(selected.error());

                            if (selected->empty()) {
                                return std::unexpected(error_at(consumer.location_of("resolver"), "consumer resolver cannot be empty"));
                            }
                            resolver = std::move(*selected);
                            consumer.take_all();
                        }

                        auto kind = package.optional_string("kind");
                        if (!kind)
                            return std::unexpected(kind.error());

                        if (*kind && **kind != "source-only") {
                            return std::unexpected(
                                error_at(package.location_of("kind"), "unknown package-map package kind `" + **kind + "`")
                            );
                        }

                        auto source = read_locator(package.take("source"), "package source");
                        if (!source)
                            return std::unexpected(source.error());

                        auto artifact = read_locator(package.take("artifact"), "package artifact");
                        if (!artifact)
                            return std::unexpected(artifact.error());

                        if (*source && *artifact) {
                            return std::unexpected(
                                error_at((*entries)[index].location(), "package cannot declare both `source` and `artifact`")
                            );
                        }

                        if (*kind) {
                            if (!*source) {
                                return std::unexpected(
                                    error_at(package.location_of("kind"), "source-only package requires a `source` locator")
                                );
                            }
                            if (has_consumer) {
                                return std::unexpected(
                                    error_at(package.location_of("consumer"), "source-only package cannot declare a consumer")
                                );
                            }
                        }

                        package.take_all();
                        candidates.push_back(
                            {std::move(*name),
                                std::move(version),
                                definition.name,
                                std::move(*source),
                                std::move(resolver),
                                (*entries)[index],
                                std::move(*artifact)}
                        );
                    }
                }

                options.take_all();
                ProviderInfo provider_info{definition.name, m_name, definition.is_default};
                std::unique_ptr<PackageProvider> provider = std::make_unique<DescriptiveProvider>(
                    std::move(provider_info),
                    std::move(candidates)
                );
                return provider;
            }

        private:
            std::string m_name;
            std::string m_description;
            bool m_accepts_packages = false;
        };
    }

    std::unique_ptr<ProviderDriver> make_provider_driver() {
        return std::make_unique<PathProviderDriver>();
    }

    std::unique_ptr<ProviderDriver> make_package_map_provider_driver() {
        return std::make_unique<DescriptiveProviderDriver>("package-map", "provides project-defined package descriptors", true);
    }

    std::unique_ptr<ProviderDriver> make_system_packages_provider_driver() {
        return std::make_unique<DescriptiveProviderDriver>("system-packages", "provides platform package descriptors", true);
    }

    std::unique_ptr<ProviderDriver> make_registry_provider_driver() {
        return std::make_unique<DescriptiveProviderDriver>("kaixa-registry", "describes a registry without synchronizing it", false);
    }
}
