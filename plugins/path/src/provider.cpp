#include <kaixa/plugin/path/provider.hpp>

#include <kaixa/config/table_reader.hpp>
#include <kaixa/workspace/package_index.hpp>

#include <system_error>
#include <utility>

namespace kaixa::plugin::path {
    namespace {
        SourceLocator source_locator(const std::filesystem::path& root, const SourceLocation& location) {
            return {
                "path",
                Value::table({
                    {"path", Value::string(root.generic_string(), location)}
                }, location)
            };
        }

        class PathProvider final : public PackageProvider {
        public:
            PathProvider(ProviderInfo info, std::filesystem::path root, SourceLocation location)
                : m_info(std::move(info)), m_root(std::move(root)), m_location(std::move(location)) {
            }

            [[nodiscard]] ProviderInfo info() const override {
                return m_info;
            }

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
                        "package `" + package->name + "` exposed by provider `" + m_info.name
                            + "` does not declare a version"
                    ));
                }

                return std::vector{PackageCandidate{
                    package->name,
                    *package->version,
                    m_info.name,
                    source_locator(m_root, m_location)
                }};
            }

        private:
            Result<std::vector<PackageCandidate>> candidate(const Manifest& package) const {
                if (!package.version) {
                    return std::unexpected(error_at(
                        package.location,
                        "package `" + package.name + "` exposed by provider `" + m_info.name
                            + "` does not declare a version"
                    ));
                }

                return std::vector{PackageCandidate{
                    package.name,
                    *package.version,
                    m_info.name,
                    source_locator(m_root, m_location)
                }};
            }

            ProviderInfo m_info;
            std::filesystem::path m_root;
            SourceLocation m_location;
        };

        class PathProviderDriver final : public ProviderDriver {
        public:
            [[nodiscard]] ProviderDriverInfo info() const override {
                return {"path", "exposes packages from a local source tree"};
            }

            [[nodiscard]] Result<std::unique_ptr<PackageProvider>> create(
                const ProviderDefinition& definition,
                const ProviderContext& context
            ) const override {
                auto options_result = TableReader::bind(definition.options, "providers." + definition.name);
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
    }

    std::unique_ptr<ProviderDriver> make_provider_driver() {
        return std::make_unique<PathProviderDriver>();
    }
}
