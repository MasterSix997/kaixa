#include <kaixa/model/manifest.hpp>

#include <kaixa/foundation/filesystem.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string_view>

namespace kaixa {
    namespace {
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
            std::string result;
            result.push_back('"');
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

        std::string key(const std::string_view name) {
            return is_bare_key(name) ? std::string(name) : toml_string(name);
        }

        std::string_view target_section(const PackageTarget& target) {
            switch (target.kind) {
                case PackageTargetKind::test: return target.each_source ? "tests" : "test";
                case PackageTargetKind::example: return target.each_source ? "examples" : "example";
                case PackageTargetKind::benchmark:
                    return target.each_source ? "benchmarks" : "benchmark";
            }
            return "target";
        }

        std::string_view reference_key(const PackageTargetKind kind) {
            switch (kind) {
                case PackageTargetKind::test: return "tests";
                case PackageTargetKind::example: return "examples";
                case PackageTargetKind::benchmark: return "benchmarks";
            }
            return "targets";
        }

        void append_strings(std::string& output, const std::string_view name, const std::vector<std::string>& values) {
            output += key(name) + " = [";
            for (std::size_t index = 0; index < values.size(); ++index) {
                if (index != 0)
                    output += ", ";

                output += toml_string(values[index]);
            }
            output += "]\n";
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
                if (!result.contains('.') && !result.contains('e') && !result.contains('E')) {
                    result += ".0";
                }
                return result;
            }
            if (const std::string* string = value.as_string())
                return toml_string(*string);
            if (const std::vector<Value>* array = value.as_array()) {
                std::string output = "[";
                for (std::size_t index = 0; index < array->size(); ++index) {
                    auto item = format_value((*array)[index]);
                    if (!item)
                        return std::unexpected(item.error());
                    if (index != 0)
                        output += ", ";
                    output += *item;
                }
                output += ']';
                return output;
            }
            if (const std::vector<TableEntry>* table = value.as_table()) {
                std::string output = "{ ";
                for (std::size_t index = 0; index < table->size(); ++index) {
                    const TableEntry& entry = (*table)[index];
                    const auto duplicate = std::ranges::find_if(
                        table->begin(),
                        table->begin() + static_cast<std::ptrdiff_t>(index),
                        [&](const TableEntry& candidate) { return candidate.key == entry.key; }
                    );
                    if (duplicate != table->begin() + static_cast<std::ptrdiff_t>(index)) {
                        return std::unexpected(error(
                            "duplicate manifest value key `" + entry.key + "`"
                        ));
                    }
                    auto child = format_value(entry.value);
                    if (!child)
                        return std::unexpected(child.error());
                    if (index != 0)
                        output += ", ";
                    output += key(entry.key) + " = " + *child;
                }
                output += " }";
                return output;
            }
            return std::unexpected(error("cannot write an empty manifest value"));
        }

        Result<void> append_table(std::string& output, const Value& value) {
            const std::vector<TableEntry>* entries = value.as_table();
            if (!entries)
                return std::unexpected(error("manifest resolver settings must be a table"));

            for (std::size_t index = 0; index < entries->size(); ++index) {
                const TableEntry& entry = (*entries)[index];
                const auto duplicate = std::ranges::find_if(
                    entries->begin(),
                    entries->begin() + static_cast<std::ptrdiff_t>(index),
                    [&](const TableEntry& candidate) { return candidate.key == entry.key; }
                );
                if (duplicate != entries->begin() + static_cast<std::ptrdiff_t>(index)) {
                    return std::unexpected(error(
                        "duplicate manifest value key `" + entry.key + "`"
                    ));
                }
                auto formatted = format_value(entry.value);
                if (!formatted)
                    return std::unexpected(formatted.error());

                output += key(entry.key) + " = " + *formatted + '\n';
            }
            return {};
        }

        void append_header(std::string& output, std::initializer_list<std::string_view> path);
        void append_array_header(std::string& output, std::initializer_list<std::string_view> path);

        Result<void> append_package_target(
            std::string& output,
            const PackageTarget& target,
            const std::string_view resolver,
            const bool repeated
        ) {
            const std::string_view section = target_section(target);
            if (repeated)
                append_array_header(output, {section});
            else
                append_header(output, {section});

            if (target.name)
                output += "name = " + toml_string(*target.name) + '\n';

            if (target.display_name)
                output += "display-name = " + toml_string(*target.display_name) + '\n';

            if (target.description)
                output += "description = " + toml_string(*target.description) + '\n';

            if (target.category)
                output += "category = " + toml_string(*target.category) + '\n';

            append_strings(output, "sources", target.sources.include);
            if (!target.sources.exclude.empty())
                append_strings(output, "source-excludes", target.sources.exclude);

            if (!target.required_features.empty())
                append_strings(output, "required-features", target.required_features);

            if (!target.arguments.empty())
                append_strings(output, "arguments", target.arguments);

            if (target.discover)
                output += "discover = true\n";

            if (target.hidden)
                output += "hidden = true\n";

            if (target.resolver_options) {
                append_header(output, {section, resolver});
                auto appended = append_table(output, *target.resolver_options);
                if (!appended)
                    return std::unexpected(appended.error());
            }

            return {};
        }

        Result<void> append_resolver_document(
            std::string& output,
            const std::string_view resolver,
            const Value& value
        ) {
            const std::vector<TableEntry>* entries = value.as_table();
            if (!entries)
                return std::unexpected(error("manifest resolver settings must be a table"));

            append_header(output, {resolver});
            for (std::size_t index = 0; index < entries->size(); ++index) {
                const TableEntry& entry = (*entries)[index];
                const auto duplicate = std::ranges::find_if(
                    entries->begin(),
                    entries->begin() + static_cast<std::ptrdiff_t>(index),
                    [&](const TableEntry& candidate) { return candidate.key == entry.key; }
                );
                if (duplicate != entries->begin() + static_cast<std::ptrdiff_t>(index)) {
                    return std::unexpected(error(
                        "duplicate manifest value key `" + entry.key + "`"
                    ));
                }

                const std::vector<Value>* array = entry.value.as_array();
                const bool tables = array && !array->empty() && std::ranges::all_of(*array,
                    [](const Value& item) {
                        return item.is_table();
                    });
                if (tables)
                    continue;

                auto formatted = format_value(entry.value);
                if (!formatted)
                    return std::unexpected(formatted.error());

                output += key(entry.key) + " = " + *formatted + '\n';
            }

            for (const TableEntry& entry: *entries) {
                const std::vector<Value>* array = entry.value.as_array();
                if (!array || array->empty() || !std::ranges::all_of(*array,
                    [](const Value& item) {
                        return item.is_table();
                    })) {
                    continue;
                }
                for (const Value& item: *array) {
                    append_array_header(output, {resolver, entry.key});
                    auto appended = append_table(output, item);
                    if (!appended)
                        return std::unexpected(appended.error());
                }
            }
            return {};
        }

        void append_header(std::string& output, const std::initializer_list<std::string_view> path) {
            if (!output.empty() && output.back() != '\n')
                output.push_back('\n');

            if (!output.empty())
                output.push_back('\n');

            output.push_back('[');
            bool first = true;
            for (const std::string_view component: path) {
                if (!first)
                    output.push_back('.');

                output += key(component);
                first = false;
            }
            output += "]\n";
        }

        void append_array_header(std::string& output, const std::initializer_list<std::string_view> path) {
            if (!output.empty() && output.back() != '\n')
                output.push_back('\n');

            if (!output.empty())
                output.push_back('\n');

            output += "[[";
            bool first = true;
            for (const std::string_view component: path) {
                if (!first)
                    output.push_back('.');

                output += key(component);
                first = false;
            }

            output += "]]\n";
        }

        Result<void> validate_manifest(const Manifest& manifest) {
            if (!is_valid_identifier(manifest.name))
                return std::unexpected(error("invalid package name `" + manifest.name + "`"));

            if (!is_valid_identifier(manifest.resolver))
                return std::unexpected(error("invalid resolver name `" + manifest.resolver + "`"));

            if (manifest.version && manifest.version->text.empty())
                return std::unexpected(error("package version cannot be empty"));

            for (std::size_t index = 0; index < manifest.dependencies.size(); ++index) {
                const DependencySpec& dependency = manifest.dependencies[index];
                if (!is_valid_identifier(dependency.name))
                    return std::unexpected(error("invalid dependency name `" + dependency.name + "`"));

                if (dependency.path.empty())
                    return std::unexpected(error("dependency `" + dependency.name + "` has an empty path"));

                const auto duplicate = std::ranges::find_if(
                    manifest.dependencies.begin(),
                    manifest.dependencies.begin() + static_cast<std::ptrdiff_t>(index),
                    [&](const DependencySpec& candidate) { return candidate.name == dependency.name; }
                );
                if (duplicate != manifest.dependencies.begin() + static_cast<std::ptrdiff_t>(index))
                    return std::unexpected(error("duplicate dependency `" + dependency.name + "`"));
            }
            if (manifest.resolver_options && !manifest.resolver_options->is_table())
                return std::unexpected(error("manifest resolver options must be a table"));

            for (const PackageTargetReference& reference: manifest.target_references) {
                if (reference.path.empty())
                    return std::unexpected(error("package target manifest path cannot be empty"));
            }
            for (const PackageTarget& target: manifest.targets) {
                if (target.name && !is_valid_identifier(*target.name))
                    return std::unexpected(error("invalid package target name `" + *target.name + "`"));

                if (target.sources.include.empty())
                    return std::unexpected(error("package target requires source patterns"));

                if (target.resolver_options && !target.resolver_options->is_table()) {
                    return std::unexpected(error(
                        "package target resolver options must be a table"
                    ));
                }
            }

            for (std::size_t index = 0; index < manifest.configurations.definitions.size(); ++index) {
                const ConfigurationDefinition& configuration = manifest.configurations.definitions[index];
                if (configuration.name.empty())
                    return std::unexpected(error("build configuration name cannot be empty"));

                const auto duplicate = std::ranges::find_if(
                    manifest.configurations.definitions.begin(),
                    manifest.configurations.definitions.begin() + static_cast<std::ptrdiff_t>(index),
                    [&](const ConfigurationDefinition& candidate) {
                        return candidate.name == configuration.name;
                    }
                );
                if (duplicate != manifest.configurations.definitions.begin()
                    + static_cast<std::ptrdiff_t>(index)) {
                    return std::unexpected(error(
                        "duplicate build configuration `" + configuration.name + "`"
                    ));
                }

                for (std::size_t resolver_index = 0;
                     resolver_index < configuration.resolvers.size();
                     ++resolver_index) {
                    const ResolverConfigurationDefinition& resolver =
                        configuration.resolvers[resolver_index];
                    if (!is_valid_identifier(resolver.resolver)) {
                        return std::unexpected(error(
                            "invalid resolver name `" + resolver.resolver
                                + "` in build configuration `" + configuration.name + "`"
                        ));
                    }
                    if (!resolver.settings.is_table()) {
                        return std::unexpected(error(
                            "settings for resolver `" + resolver.resolver + "` must be a table"
                        ));
                    }
                    const auto duplicate_resolver = std::ranges::find_if(
                        configuration.resolvers.begin(),
                        configuration.resolvers.begin()
                            + static_cast<std::ptrdiff_t>(resolver_index),
                        [&](const ResolverConfigurationDefinition& candidate) {
                            return candidate.resolver == resolver.resolver;
                        }
                    );
                    if (duplicate_resolver != configuration.resolvers.begin()
                        + static_cast<std::ptrdiff_t>(resolver_index)) {
                        return std::unexpected(error(
                            "duplicate resolver `" + resolver.resolver
                                + "` in build configuration `" + configuration.name + "`"
                        ));
                    }
                }
            }
            return {};
        }
    }

    Result<std::string> format_manifest(const Manifest& manifest) {
        auto valid = validate_manifest(manifest);
        if (!valid)
            return std::unexpected(valid.error());

        std::string output = "[package]\nname = " + toml_string(manifest.name) + '\n';
        if (manifest.version)
            output += "version = " + toml_string(manifest.version->text) + '\n';

        output += "resolver = " + toml_string(manifest.resolver) + '\n';

        for (const PackageTargetKind kind: {
                 PackageTargetKind::test,
                 PackageTargetKind::example,
                 PackageTargetKind::benchmark
             }) {
            std::vector<std::string> paths;
            for (const PackageTargetReference& reference: manifest.target_references) {
                if (reference.kind == kind)
                    paths.push_back(reference.path.generic_string());
            }
            if (!paths.empty())
                append_strings(output, reference_key(kind), paths);
        }

        if (!manifest.dependencies.empty()) {
            append_header(output, {"dependencies"});
            for (const DependencySpec& dependency: manifest.dependencies) {
                output += key(dependency.name) + " = { path = "
                    + toml_string(dependency.path.generic_string()) + " }\n";
            }
        }

        if (!manifest.configurations.defaults.empty() || !manifest.configurations.definitions.empty()) {
            if (!manifest.configurations.defaults.empty()) {
                append_header(output, {"build"});
                output += "default-configs = [";
                for (std::size_t index = 0; index < manifest.configurations.defaults.size(); ++index) {
                    if (index != 0)
                        output += ", ";

                    output += toml_string(manifest.configurations.defaults[index]);
                }
                output += "]\n";
            }

            for (const ConfigurationDefinition& configuration: manifest.configurations.definitions) {
                append_array_header(output, {"config"});
                output += "name = " + toml_string(configuration.name) + '\n';
                if (configuration.profile)
                    output += "profile = " + toml_string(*configuration.profile) + '\n';

                for (const ResolverConfigurationDefinition& resolver: configuration.resolvers) {
                    append_header(output, {"config", resolver.resolver});
                    auto appended = append_table(output, resolver.settings);
                    if (!appended)
                        return std::unexpected(appended.error());
                }
            }
        }

        if (manifest.resolver_options) {
            auto appended = append_resolver_document(
                output,
                manifest.resolver,
                *manifest.resolver_options
            );
            if (!appended)
                return std::unexpected(appended.error());
        }

        for (const PackageTarget& target: manifest.targets) {
            const std::string_view section = target_section(target);
            const std::size_t count = static_cast<std::size_t>(std::ranges::count_if(
                manifest.targets,
                [&](const PackageTarget& candidate) {
                    return target_section(candidate) == section;
                }
            ));
            auto appended = append_package_target(
                output,
                target,
                manifest.resolver,
                count > 1
            );
            if (!appended)
                return std::unexpected(appended.error());
        }
        return output;
    }

    Result<void> write_manifest_file(const std::filesystem::path& path, const Manifest& manifest) {
        auto contents = format_manifest(manifest);
        if (!contents)
            return std::unexpected(contents.error());

        return write_file(path, *contents);
    }
}
