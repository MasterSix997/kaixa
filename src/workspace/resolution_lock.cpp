#include <kaixa/workspace/resolution_lock.hpp>

#include <kaixa/config/parser.hpp>
#include <kaixa/config/table_reader.hpp>
#include <kaixa/foundation/filesystem.hpp>
#include <kaixa/model/policy.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace kaixa {
    namespace {
        constexpr std::int64_t lock_schema = 1;

        bool is_not_found(const std::error_code& failure) {
            if (failure == std::errc::no_such_file_or_directory)
                return true;

#ifdef _WIN32
            return failure.value() == ERROR_FILE_NOT_FOUND || failure.value() == ERROR_PATH_NOT_FOUND;
#else
            return false;
#endif
        }

        bool is_bare_key(const std::string_view key) {
            return !key.empty() && std::ranges::all_of(key, [](const char character) {
                return (character >= 'a' && character <= 'z')
                    || (character >= 'A' && character <= 'Z')
                    || (character >= '0' && character <= '9')
                    || character == '_'
                    || character == '-';
            });
        }

        std::string toml_string(const std::string_view text) {
            constexpr char hexadecimal[] = "0123456789ABCDEF";
            std::string result{"\""};
            for (const char character: text) {
                switch (character) {
                case '\b': result += "\\b"; break;
                case '\t': result += "\\t"; break;
                case '\n': result += "\\n"; break;
                case '\f': result += "\\f"; break;
                case '\r': result += "\\r"; break;
                case '"': result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                default: {
                    const auto byte = static_cast<unsigned char>(character);
                    if (byte < 0x20U || byte == 0x7FU) {
                        result += "\\u00";
                        result.push_back(hexadecimal[byte >> 4U]);
                        result.push_back(hexadecimal[byte & 0x0FU]);
                    } else {
                        result.push_back(character);
                    }
                }
                }
            }
            result.push_back('"');
            return result;
        }

        std::string toml_key(const std::string_view key) {
            return is_bare_key(key) ? std::string(key) : toml_string(key);
        }

        Result<std::string> format_value(const Value& value) {
            if (const bool* boolean = value.as_boolean())
                return *boolean ? "true" : "false";

            if (const std::int64_t* integer = value.as_integer())
                return std::to_string(*integer);

            if (const double* floating = value.as_floating()) {
                if (std::isnan(*floating))
                    return "nan";

                if (std::isinf(*floating))
                    return std::signbit(*floating) ? "-inf" : "inf";

                std::ostringstream output;
                output.imbue(std::locale::classic());
                output << std::setprecision(std::numeric_limits<double>::max_digits10) << *floating;
                std::string result = output.str();
                if (!result.contains('.') && !result.contains('e') && !result.contains('E'))
                    result += ".0";

                return result;
            }
            if (const std::string* string = value.as_string())
                return toml_string(*string);

            if (const std::vector<Value>* array = value.as_array()) {
                std::string output{"["};
                for (std::size_t index = 0; index < array->size(); ++index) {
                    auto formatted = format_value((*array)[index]);
                    if (!formatted)
                        return std::unexpected(formatted.error());

                    if (index != 0)
                        output += ", ";

                    output += *formatted;
                }
                output += ']';
                return output;
            }
            if (const std::vector<TableEntry>* table = value.as_table()) {
                std::vector<const TableEntry*> entries;
                entries.reserve(table->size());
                for (const TableEntry& entry: *table)
                    entries.push_back(&entry);

                std::ranges::sort(entries, {}, [](const TableEntry* entry) { return entry->key; });
                std::string output{"{ "};
                for (std::size_t index = 0; index < entries.size(); ++index) {
                    if (index != 0 && entries[index - 1]->key == entries[index]->key) {
                        return std::unexpected(error("duplicate lock value key `" + entries[index]->key + "`"));
                    }
                    if (index != 0)
                        output += ", ";

                    auto formatted = format_value(entries[index]->value);
                    if (!formatted)
                        return std::unexpected(formatted.error());

                    output += toml_key(entries[index]->key) + " = " + *formatted;
                }
                output += " }";
                return output;
            }
            return std::unexpected(error("cannot write an empty lock value"));
        }

        void append_size(std::string& output, const std::size_t size) {
            output += std::to_string(size);
            output += ':';
        }

        void append_canonical_value(std::string& output, const Value& value) {
            output += static_cast<char>('0' + static_cast<int>(value.kind()));
            if (const bool* boolean = value.as_boolean()) {
                output += *boolean ? '1' : '0';
            } else if (const std::int64_t* integer = value.as_integer()) {
                output += std::to_string(*integer);
            } else if (const double* floating = value.as_floating()) {
                std::ostringstream text;
                text.imbue(std::locale::classic());
                text << std::setprecision(std::numeric_limits<double>::max_digits10) << *floating;
                output += text.str();
            } else if (const std::string* string = value.as_string()) {
                append_size(output, string->size());
                output += *string;
            } else if (const std::vector<Value>* array = value.as_array()) {
                append_size(output, array->size());
                for (const Value& item: *array)
                    append_canonical_value(output, item);

            } else if (const std::vector<TableEntry>* table = value.as_table()) {
                std::vector<const TableEntry*> entries;
                entries.reserve(table->size());
                for (const TableEntry& entry: *table)
                    entries.push_back(&entry);

                std::ranges::sort(entries, {}, [](const TableEntry* entry) { return entry->key; });
                append_size(output, entries.size());
                for (const TableEntry* entry: entries) {
                    append_size(output, entry->key.size());
                    output += entry->key;
                    append_canonical_value(output, entry->value);
                }
            }
        }

        std::string canonical_value(const Value& value) {
            std::string result;
            append_canonical_value(result, value);
            return result;
        }

        std::string portable_path(const std::string_view text, const std::filesystem::path& context_directory) {
            const std::filesystem::path path{text};
            if (!path.is_absolute())
                return path.generic_string();

            const std::filesystem::path relative = path.lexically_normal().lexically_relative(context_directory.lexically_normal());
            if (relative.empty() || *relative.begin() == "..")
                return path.lexically_normal().generic_string();

            return (std::filesystem::path{"."} / relative).generic_string();
        }

        Value normalized_source_options(const SourceLocator& source, const std::filesystem::path& context_directory) {
            if (source.driver != "path")
                return source.options;

            const std::vector<TableEntry>* table = source.options.as_table();
            if (!table)
                return source.options;

            std::vector<TableEntry> result = *table;
            for (TableEntry& entry: result) {
                if (entry.key == "path" && entry.value.as_string()) {
                    entry.value = Value::string(portable_path(*entry.value.as_string(), context_directory), entry.value.location());
                }
            }
            return Value::table(std::move(result), source.options.location());
        }

        std::optional<std::string> normalized_identity(const PackageSource& source, const std::filesystem::path& context_directory) {
            if (!source.identity)
                return std::nullopt;

            if (source.locator && source.locator->driver == "path")
                return portable_path(*source.identity, context_directory);

            return source.identity;
        }

        Result<std::vector<std::string>> string_array(TableReader& table, const std::string_view key, const bool required = true) {
            const Value* value = table.take(key);
            if (!value) {
                if (required)
                    return std::unexpected(error_at(table.location_of(key), "missing required key"));

                return std::vector<std::string>{};
            }

            const std::vector<Value>* array = value->as_array();
            if (!array)
                return std::unexpected(error_at(value->location(), "expected an array of strings"));

            std::vector<std::string> result;
            result.reserve(array->size());
            for (const Value& item: *array) {
                if (!item.as_string())
                    return std::unexpected(error_at(item.location(), "expected a string"));

                result.push_back(*item.as_string());
            }
            return result;
        }

        Result<std::vector<LockedVariant>> parse_variants(TableReader& resolution) {
            const Value* value = resolution.take("variants");
            if (!value)
                return std::unexpected(error_at(resolution.location_of("variants"), "missing required key"));

            const std::vector<Value>* entries = value->as_array();
            if (!entries)
                return std::unexpected(error_at(value->location(), "expected an array of variant tables"));

            std::vector<LockedVariant> result;
            result.reserve(entries->size());
            for (std::size_t index = 0; index < entries->size(); ++index) {
                auto variant_result = TableReader::bind((*entries)[index], resolution.path() + ".variants." + std::to_string(index));
                if (!variant_result)
                    return std::unexpected(variant_result.error());

                TableReader variant = std::move(*variant_result);
                auto features = string_array(variant, "features");
                auto policy = variant.string("policy");
                auto contexts = string_array(variant, "contexts");
                if (!features)
                    return std::unexpected(features.error());

                if (!policy)
                    return std::unexpected(policy.error());

                if (!contexts)
                    return std::unexpected(contexts.error());

                auto finished = variant.finish();
                if (!finished)
                    return std::unexpected(finished.error());

                result.push_back({std::move(*features), std::move(*policy), std::move(*contexts)});
            }
            return result;
        }

        Result<std::vector<LockedPackageResolution>> parse_resolutions(TableReader& package) {
            const Value* value = package.take("resolutions");
            if (!value)
                return std::unexpected(error_at(package.location_of("resolutions"), "missing required key"));

            const std::vector<Value>* entries = value->as_array();
            if (!entries)
                return std::unexpected(error_at(value->location(), "expected an array of resolution tables"));

            std::vector<LockedPackageResolution> result;
            result.reserve(entries->size());
            for (std::size_t index = 0; index < entries->size(); ++index) {
                auto resolution_result = TableReader::bind((*entries)[index], package.path() + ".resolutions." + std::to_string(index));
                if (!resolution_result)
                    return std::unexpected(resolution_result.error());

                TableReader resolution = std::move(*resolution_result);
                auto roots = string_array(resolution, "roots");
                auto profile = resolution.string("profile");
                auto target = resolution.string("target");
                auto features = string_array(resolution, "features");
                auto dependencies = string_array(resolution, "dependencies");
                auto variants = parse_variants(resolution);
                if (!roots)
                    return std::unexpected(roots.error());

                if (!profile)
                    return std::unexpected(profile.error());

                if (!target)
                    return std::unexpected(target.error());

                if (!features)
                    return std::unexpected(features.error());

                if (!dependencies)
                    return std::unexpected(dependencies.error());

                if (!variants)
                    return std::unexpected(variants.error());

                auto finished = resolution.finish();
                if (!finished)
                    return std::unexpected(finished.error());

                result.push_back(
                    {std::move(*roots),
                        std::move(*profile),
                        std::move(*target),
                        std::move(*features),
                        std::move(*dependencies),
                        std::move(*variants)}
                );
            }
            return result;
        }

        Result<LockedPackage> parse_locked_package(const Value& value, const std::size_t index) {
            auto package_result = TableReader::bind(value, "package." + std::to_string(index));
            if (!package_result)
                return std::unexpected(package_result.error());

            TableReader package = std::move(*package_result);
            auto name = package.string("name");
            auto version = package.optional_string("version");
            auto resolver = package.string("resolver");
            auto provider = package.optional_string("provider");
            auto authority = package.optional_string("authority");
            auto source_driver = package.optional_string("source-driver");
            const Value* source_options = package.take("source-options");
            auto source_identity = package.optional_string("source-identity");
            auto resolutions = parse_resolutions(package);
            if (!name)
                return std::unexpected(name.error());

            if (!version)
                return std::unexpected(version.error());

            if (!resolver)
                return std::unexpected(resolver.error());

            if (!provider)
                return std::unexpected(provider.error());

            if (!authority)
                return std::unexpected(authority.error());

            if (!source_driver)
                return std::unexpected(source_driver.error());

            if (!source_identity)
                return std::unexpected(source_identity.error());

            if (!resolutions)
                return std::unexpected(resolutions.error());

            if (source_options && !source_options->is_table()) {
                return std::unexpected(error_at(source_options->location(), "lock source options must be a table"));
            }
            if (source_options && !*source_driver) {
                return std::unexpected(error_at(source_options->location(), "lock source options require `source-driver`"));
            }
            if (*source_driver && !source_options) {
                return std::unexpected(error_at(package.location_of("source-options"), "missing required key"));
            }

            auto finished = package.finish();
            if (!finished)
                return std::unexpected(finished.error());

            return LockedPackage{std::move(*name),
                std::move(*version),
                std::move(*resolver),
                std::move(*provider),
                std::move(*authority),
                std::move(*source_driver),
                source_options ? std::optional<Value>{*source_options} : std::nullopt,
                std::move(*source_identity),
                std::move(*resolutions)};
        }

        std::string strings_value(const std::span<const std::string> values) {
            std::string output{"["};
            for (std::size_t index = 0; index < values.size(); ++index) {
                if (index != 0)
                    output += ", ";

                output += toml_string(values[index]);
            }
            output += ']';
            return output;
        }

        std::string variant_value(const LockedVariant& variant) {
            return "{ features = "
                + strings_value(variant.features)
                + ", policy = "
                + toml_string(variant.policy)
                + ", contexts = "
                + strings_value(variant.contexts)
                + " }";
        }

        std::string resolution_value(const LockedPackageResolution& resolution) {
            std::string variants{"["};
            for (std::size_t index = 0; index < resolution.variants.size(); ++index) {
                if (index != 0)
                    variants += ", ";

                variants += variant_value(resolution.variants[index]);
            }
            variants += ']';
            return "{ roots = "
                + strings_value(resolution.roots)
                + ", profile = "
                + toml_string(resolution.profile)
                + ", target = "
                + toml_string(resolution.target)
                + ", features = "
                + strings_value(resolution.features)
                + ", dependencies = "
                + strings_value(resolution.dependencies)
                + ", variants = "
                + variants
                + " }";
        }

        bool same_package_identity(const LockedPackage& left, const LockedPackage& right) {
            return left.name == right.name
                && left.version == right.version
                && left.resolver == right.resolver
                && left.provider == right.provider
                && left.authority == right.authority
                && left.source_driver == right.source_driver
                && left.source_identity == right.source_identity
                && left.source_options.has_value() == right.source_options.has_value()
                && (!left.source_options || canonical_value(*left.source_options) == canonical_value(*right.source_options));
        }

        bool same_resolution_key(const LockedPackageResolution& left, const LockedPackageResolution& right) {
            return left.roots == right.roots && left.profile == right.profile && left.target == right.target;
        }

        std::string package_version(const LockedPackage& package) {
            return package.version.value_or("unversioned");
        }

        Result<void> replace_file(const std::filesystem::path& temporary, const std::filesystem::path& destination) {
#ifdef _WIN32
            if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                return std::unexpected(
                    error_at({destination.string(), 0, 0, {}}, "cannot replace lockfile: system error " + std::to_string(GetLastError()))
                );
            }
#else
            std::error_code failure;
            std::filesystem::rename(temporary, destination, failure);
            if (failure) {
                return std::unexpected(error_at({destination.string(), 0, 0, {}}, "cannot replace lockfile: " + failure.message()));
            }
#endif
            return {};
        }
    }

    const LockedPackage* ResolutionLock::find(const std::string_view package) const noexcept {
        const auto result = std::ranges::find(packages, package, &LockedPackage::name);
        return result == packages.end() ? nullptr : &*result;
    }

    Result<std::optional<ResolutionLock>> read_resolution_lock(const std::filesystem::path& path) {
        std::error_code failure;
        const bool exists = std::filesystem::exists(path, failure);
        if (is_not_found(failure))
            failure.clear();

        if (failure)
            return std::unexpected(error("cannot inspect lockfile `" + path.string() + "`: " + failure.message()));

        if (!exists)
            return std::optional<ResolutionLock>{};

        auto document = parse_file(path);
        if (!document)
            return std::unexpected(document.error());

        auto root_result = TableReader::bind(*document);
        if (!root_result)
            return std::unexpected(root_result.error());

        TableReader root = std::move(*root_result);
        const Value* schema = root.take("schema");
        if (!schema || !schema->as_integer())
            return std::unexpected(error_at(root.location_of("schema"), "lockfile requires an integer `schema`"));

        if (*schema->as_integer() != lock_schema) {
            return std::unexpected(
                error_at(schema->location(), "unsupported Kaixa.lock schema `" + std::to_string(*schema->as_integer()) + "`")
            );
        }

        const Value* packages = root.take("package");
        if (!packages || !packages->as_array())
            return std::unexpected(error_at(root.location_of("package"), "lockfile requires a `package` array"));

        ResolutionLock result;
        result.packages.reserve(packages->as_array()->size());
        for (std::size_t index = 0; index < packages->as_array()->size(); ++index) {
            auto package = parse_locked_package((*packages->as_array())[index], index);
            if (!package)
                return std::unexpected(package.error());

            if (result.find(package->name)) {
                return std::unexpected(
                    error_at((*packages->as_array())[index].location(), "duplicate locked package `" + package->name + "`")
                );
            }
            result.packages.push_back(std::move(*package));
        }

        auto finished = root.finish();
        if (!finished)
            return std::unexpected(finished.error());

        return std::optional{std::move(result)};
    }

    Result<std::string> format_resolution_lock(const ResolutionLock& lock) {
        std::vector<const LockedPackage*> packages;
        packages.reserve(lock.packages.size());
        for (const LockedPackage& package: lock.packages)
            packages.push_back(&package);

        std::ranges::sort(packages, {}, [](const LockedPackage* package) { return package->name; });
        std::string output = "# Generated by Kaixa. Do not edit manually.\nschema = " + std::to_string(lock_schema) + "\n";
        for (const LockedPackage* package: packages) {
            output += "\n[[package]]\nname = " + toml_string(package->name) + '\n';
            if (package->version)
                output += "version = " + toml_string(*package->version) + '\n';

            output += "resolver = " + toml_string(package->resolver) + '\n';
            if (package->provider)
                output += "provider = " + toml_string(*package->provider) + '\n';

            if (package->authority)
                output += "authority = " + toml_string(*package->authority) + '\n';

            if (package->source_driver)
                output += "source-driver = " + toml_string(*package->source_driver) + '\n';

            if (package->source_options) {
                auto options = format_value(*package->source_options);
                if (!options)
                    return std::unexpected(options.error());

                output += "source-options = " + *options + '\n';
            }
            if (package->source_identity)
                output += "source-identity = " + toml_string(*package->source_identity) + '\n';

            output += "resolutions = [\n";
            for (const LockedPackageResolution& resolution: package->resolutions)
                output += "    " + resolution_value(resolution) + ",\n";

            output += "]\n";
        }
        return output;
    }

    Result<bool> write_resolution_lock(const std::filesystem::path& path, const ResolutionLock& lock) {
        auto contents = format_resolution_lock(lock);
        if (!contents)
            return std::unexpected(contents.error());

        std::error_code failure;
        const bool exists = std::filesystem::is_regular_file(path, failure);
        if (is_not_found(failure))
            failure.clear();

        if (exists) {
            auto existing = read_file(path);
            if (!existing)
                return std::unexpected(existing.error());

            if (*existing == *contents)
                return false;
        } else if (failure) {
            return std::unexpected(error("cannot inspect lockfile `" + path.string() + "`: " + failure.message()));
        }

        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        std::filesystem::path temporary = path;
        temporary += ".tmp." + std::to_string(nonce);
        auto written = write_file(temporary, *contents);
        if (!written)
            return std::unexpected(written.error());

        auto replaced = replace_file(temporary, path);
        if (!replaced) {
            std::filesystem::remove(temporary, failure);
            return std::unexpected(replaced.error());
        }
        return true;
    }

    ResolutionLock capture_resolution_lock(
        const Graph& graph,
        const std::span<const ConfiguredPackageInstance> instances,
        const PolicyContext& context,
        const std::filesystem::path& context_directory
    ) {
        std::vector<std::string> roots;
        roots.reserve(graph.roots().size());
        for (const PackageId root: graph.roots())
            roots.push_back(graph[root].name);

        std::ranges::sort(roots);
        ResolutionLock result;
        result.packages.reserve(graph.size());
        for (const PackageNode& package: graph.nodes()) {
            LockedPackage locked;
            locked.name = package.name;
            locked.resolver = package.resolver;
            if (package.manifest && package.manifest->version)
                locked.version = package.manifest->version->text;

            if (package.source) {
                locked.provider = package.source->provider;
                if (!package.source->authority.empty())
                    locked.authority = package.source->authority;

                if (package.source->version)
                    locked.version = package.source->version->text;

                if (package.source->locator) {
                    locked.source_driver = package.source->locator->driver;
                    locked.source_options = normalized_source_options(*package.source->locator, context_directory);
                }
                locked.source_identity = normalized_identity(*package.source, context_directory);
            }

            LockedPackageResolution resolution;
            resolution.roots = roots;
            resolution.profile = context.profile;
            resolution.target = context.target_os;
            resolution.features = package.active_features;
            for (const PackageId dependency: package.dependencies)
                resolution.dependencies.push_back(graph[dependency].name);

            std::ranges::sort(resolution.features);
            std::ranges::sort(resolution.dependencies);
            for (const ConfiguredPackageInstance& instance: instances) {
                if (instance.package != package.id)
                    continue;

                LockedVariant variant{instance.features, policy_fingerprint(instance.policy), instance.contexts};
                std::ranges::sort(variant.features);
                std::ranges::sort(variant.contexts);
                resolution.variants.push_back(std::move(variant));
            }
            std::ranges::sort(resolution.variants, [](const LockedVariant& left, const LockedVariant& right) {
                if (left.policy != right.policy)
                    return left.policy < right.policy;

                if (left.features != right.features)
                    return left.features < right.features;

                return left.contexts < right.contexts;
            });
            locked.resolutions.push_back(std::move(resolution));
            result.packages.push_back(std::move(locked));
        }
        std::ranges::sort(result.packages, {}, &LockedPackage::name);
        return result;
    }

    ResolutionLock merge_resolution_lock(ResolutionLock existing, const ResolutionLock& current) {
        std::vector<LockedPackageResolution> current_contexts;
        for (const LockedPackage& package: current.packages) {
            for (const LockedPackageResolution& resolution: package.resolutions) {
                if (std::ranges::none_of(current_contexts, [&](const LockedPackageResolution& context) {
                        return same_resolution_key(context, resolution);
                    })) {
                    current_contexts.push_back(resolution);
                }
            }
        }
        for (LockedPackage& package: existing.packages) {
            std::erase_if(package.resolutions, [&](const LockedPackageResolution& resolution) {
                return std::ranges::any_of(current_contexts, [&](const LockedPackageResolution& context) {
                    return same_resolution_key(context, resolution);
                });
            });
        }
        std::erase_if(existing.packages, [](const LockedPackage& package) { return package.resolutions.empty(); });

        for (const LockedPackage& incoming: current.packages) {
            auto package = std::ranges::find(existing.packages, incoming.name, &LockedPackage::name);
            if (package == existing.packages.end()) {
                existing.packages.push_back(incoming);
                continue;
            }

            if (!same_package_identity(*package, incoming)) {
                *package = incoming;
                continue;
            }
            for (const LockedPackageResolution& resolution: incoming.resolutions) {
                const auto previous = std::ranges::find_if(package->resolutions, [&](const LockedPackageResolution& candidate) {
                    return same_resolution_key(candidate, resolution);
                });
                if (previous == package->resolutions.end())
                    package->resolutions.push_back(resolution);
                else
                    *previous = resolution;
            }
        }
        std::ranges::sort(existing.packages, {}, &LockedPackage::name);
        for (LockedPackage& package: existing.packages) {
            std::ranges::sort(package.resolutions, [](const LockedPackageResolution& left, const LockedPackageResolution& right) {
                if (left.roots != right.roots)
                    return left.roots < right.roots;

                if (left.profile != right.profile)
                    return left.profile < right.profile;

                return left.target < right.target;
            });
        }
        return existing;
    }

    Result<void> validate_resolution_lock(const ResolutionLock& expected, const ResolutionLock& current) {
        for (const LockedPackage& package: current.packages) {
            const LockedPackage* locked = expected.find(package.name);
            if (!locked) {
                return std::unexpected(error("Kaixa.lock does not contain package `" + package.name + "`")
                        .add_note("run the command without `--locked` or `--frozen` to update the lockfile"));
            }
            if (!same_package_identity(*locked, package)) {
                return std::unexpected(error(
                    "Kaixa.lock pins `"
                    + package.name
                    + "` to a different identity (locked "
                    + package_version(*locked)
                    + ", resolved "
                    + package_version(package)
                    + ")"
                )
                        .add_note("run the command without `--locked` or `--frozen` to update the lockfile"));
            }

            for (const LockedPackageResolution& resolution: package.resolutions) {
                const auto found = std::ranges::find_if(locked->resolutions, [&](const LockedPackageResolution& candidate) {
                    return same_resolution_key(candidate, resolution);
                });
                if (found == locked->resolutions.end()) {
                    return std::unexpected(error("Kaixa.lock has no resolution for package `" + package.name + "` in this context")
                            .add_note("run the command without `--locked` or `--frozen` to update the lockfile"));
                }
                if (*found != resolution) {
                    return std::unexpected(error("Kaixa.lock resolution for package `" + package.name + "` is stale")
                            .add_note("features, dependencies or configured variants changed")
                            .add_note("run the command without `--locked` or `--frozen` to update the lockfile"));
                }
            }
        }
        return {};
    }

    bool locked_candidate_matches(
        const LockedPackage& locked,
        const std::string_view provider,
        const PackageCandidate& candidate,
        const std::filesystem::path& context_directory
    ) {
        if (locked.provider != provider || locked.authority != candidate.authority)
            return false;

        const std::optional<std::string> version = candidate.version ? std::optional<std::string>{candidate.version->text} : std::nullopt;
        if (locked.version != version)
            return false;

        const std::optional<std::string> source_driver = candidate.source ? std::optional<std::string>{candidate.source->driver}
                                                                          : std::nullopt;
        if (locked.source_driver != source_driver)
            return false;

        if (locked.source_options.has_value() != candidate.source.has_value())
            return false;

        return !candidate.source
            || canonical_value(*locked.source_options) == canonical_value(normalized_source_options(*candidate.source, context_directory));
    }
}
