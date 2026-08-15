#include <kaixa/model/manifest.hpp>

#include <kaixa/config/parser.hpp>
#include <kaixa/config/table_reader.hpp>

#include <algorithm>
#include <array>
#include <utility>

namespace kaixa {
    namespace {
        Result<DependencyBinding> parse_dependency(const TableEntry& entry, const std::string& path);

        Result<std::string> read_identifier(TableReader& table, const std::string_view key) {
            auto value = table.string(key);
            if (!value)
                return std::unexpected(value.error());

            if (!is_valid_identifier(*value))
                return std::unexpected(error_at(
                    table.location_of(key),
                    "`" + *value + "` is not a valid name; use letters, digits, `_` and `-`"
                ));

            return *value;
        }

        Result<std::string> read_package_name(TableReader& table, const std::string_view key) {
            auto value = table.string(key);
            if (!value)
                return std::unexpected(value.error());

            if (!is_valid_package_name(*value)) {
                return std::unexpected(error_at(
                    table.location_of(key),
                    "`" + *value + "` is not a valid package name"
                ));
            }
            return *value;
        }

        Result<std::vector<std::string>> read_string_array(TableReader& table, const std::string_view key) {
            const Value* value = table.take(key);
            if (!value)
                return std::vector<std::string>{};

            const std::vector<Value>* array = value->as_array();
            if (!array) {
                return std::unexpected(error_at(
                    table.location_of(key),
                    "expected an array, found " + std::string(value_kind_name(value->kind()))
                ));
            }

            std::vector<std::string> result;
            result.reserve(array->size());
            for (const Value& item: *array) {
                const std::string* text = item.as_string();
                if (!text)
                    return std::unexpected(error_at(item.location(), "expected a string array element"));

                if (text->empty())
                    return std::unexpected(error_at(item.location(), "array values cannot be empty"));

                result.push_back(*text);
            }

            return result;
        }

        Result<bool> read_boolean(TableReader& table, const std::string_view key, const bool default_value = false) {
            const Value* value = table.take(key);
            if (!value)
                return default_value;

            const bool* boolean = value->as_boolean();
            if (!boolean) {
                return std::unexpected(error_at(
                    table.location_of(key),
                    "expected a boolean, found " + std::string(value_kind_name(value->kind()))
                ));
            }

            return *boolean;
        }

        Result<std::vector<DependencyBinding>> read_dependencies(TableReader& table) {
            auto dependencies_result = table.optional_table("dependencies");
            if (!dependencies_result)
                return std::unexpected(dependencies_result.error());

            std::vector<DependencyBinding> result;
            if (!*dependencies_result)
                return result;

            TableReader dependencies = std::move(**dependencies_result);
            result.reserve(dependencies.entries().size());
            for (const TableEntry& entry: dependencies.entries()) {
                auto dependency = parse_dependency(
                    entry,
                    join_config_path(dependencies.path(), entry.key)
                );
                if (!dependency)
                    return std::unexpected(dependency.error());

                result.push_back(std::move(*dependency));
            }
            dependencies.take_all();
            return result;
        }

        Result<void> read_target_references(
            TableReader& package,
            const std::string_view key,
            const PackageTargetKind kind,
            std::vector<PackageTargetReference>& output
        ) {
            auto paths = read_string_array(package, key);
            if (!paths)
                return std::unexpected(paths.error());

            for (std::string& path: *paths) {
                output.push_back({
                    kind,
                    std::filesystem::path(std::move(path)),
                    package.location_of(key)
                });
            }

            return {};
        }

        Result<PackageTarget> parse_package_target(
            TableReader& table,
            const PackageTargetKind kind,
            const bool each_source,
            const std::string_view resolver
        ) {
            PackageTarget target;
            target.kind = kind;
            target.each_source = each_source;
            target.location = table.location_of("sources");

            auto name = table.optional_string("name");
            if (!name)
                return std::unexpected(name.error());

            if (*name) {
                if (!is_valid_target_name(**name)) {
                    return std::unexpected(error_at(
                        table.location_of("name"),
                        "`" + **name + "` is not a valid target name"
                    ));
                }
                target.name = std::move(**name);
            }

            auto display_name = table.optional_string("display-name");
            if (!display_name)
                return std::unexpected(display_name.error());

            target.display_name = std::move(*display_name);

            auto description = table.optional_string("description");
            if (!description)
                return std::unexpected(description.error());

            target.description = std::move(*description);

            auto category = table.optional_string("category");
            if (!category)
                return std::unexpected(category.error());

            target.category = std::move(*category);

            auto sources = read_string_array(table, "sources");
            if (!sources)
                return std::unexpected(sources.error());

            if (sources->empty()) {
                return std::unexpected(error_at(
                    table.location_of("sources"),
                    "a package target requires at least one source pattern"
                ));
            }
            target.sources.include = std::move(*sources);
            target.sources.location = table.location_of("sources");

            auto excludes = read_string_array(table, "source-excludes");
            if (!excludes)
                return std::unexpected(excludes.error());

            target.sources.exclude = std::move(*excludes);

            auto required_features = read_string_array(table, "required-features");
            if (!required_features)
                return std::unexpected(required_features.error());

            target.required_features = std::move(*required_features);

            auto dependencies = read_dependencies(table);
            if (!dependencies)
                return std::unexpected(dependencies.error());

            target.dependencies = std::move(*dependencies);

            auto arguments = read_string_array(table, "arguments");
            if (!arguments)
                return std::unexpected(arguments.error());

            target.arguments = std::move(*arguments);

            auto discover = read_boolean(table, "discover");
            if (!discover)
                return std::unexpected(discover.error());

            target.discover = *discover;

            auto hidden = read_boolean(table, "hidden");
            if (!hidden)
                return std::unexpected(hidden.error());

            target.hidden = *hidden;

            if (const Value* options = table.take(resolver)) {
                if (!options->is_table()) {
                    return std::unexpected(error_at(
                        options->location(),
                        "target resolver options must be a table"
                    ));
                }
                target.resolver_options = *options;
            }

            auto finished = table.finish();
            if (!finished)
                return std::unexpected(finished.error());

            return target;
        }

        Result<void> parse_target_collection(
            TableReader& root,
            const std::string_view key,
            const PackageTargetKind kind,
            const bool each_source,
            const std::string_view resolver,
            std::vector<PackageTarget>& output
        ) {
            const Value* value = root.take(key);
            if (!value)
                return {};

            if (value->is_table()) {
                auto table_result = TableReader::bind(*value, std::string(key));
                if (!table_result)
                    return std::unexpected(table_result.error());

                TableReader table = std::move(*table_result);
                auto target = parse_package_target(table, kind, each_source, resolver);
                if (!target)
                    return std::unexpected(target.error());

                output.push_back(std::move(*target));
                return {};
            }

            const std::vector<Value>* array = value->as_array();
            if (!array) {
                return std::unexpected(error_at(
                    root.location_of(key),
                    "expected a target table or array of target tables"
                ));
            }

            for (std::size_t index = 0; index < array->size(); ++index) {
                auto table_result = TableReader::bind(
                    (*array)[index],
                    std::string(key) + "." + std::to_string(index)
                );
                if (!table_result)
                    return std::unexpected(table_result.error());

                TableReader table = std::move(*table_result);
                auto target = parse_package_target(table, kind, each_source, resolver);
                if (!target)
                    return std::unexpected(target.error());

                output.push_back(std::move(*target));
            }

            return {};
        }

        Result<void> parse_package_targets(
            TableReader& root,
            const std::string_view resolver,
            std::vector<PackageTarget>& output,
            const std::optional<PackageTargetKind> only = std::nullopt
        ) {
            struct TargetSection {
                std::string_view singular;
                std::string_view plural;
                PackageTargetKind kind;
            };
            constexpr std::array sections{
                TargetSection{"test", "tests", PackageTargetKind::test},
                TargetSection{"example", "examples", PackageTargetKind::example},
                TargetSection{"benchmark", "benchmarks", PackageTargetKind::benchmark}
            };

            for (const TargetSection& section: sections) {
                if (only && section.kind != *only)
                    continue;

                auto singular = parse_target_collection(
                    root,
                    section.singular,
                    section.kind,
                    false,
                    resolver,
                    output
                );
                if (!singular)
                    return std::unexpected(singular.error());

                auto plural = parse_target_collection(
                    root,
                    section.plural,
                    section.kind,
                    true,
                    resolver,
                    output
                );
                if (!plural)
                    return std::unexpected(plural.error());
            }

            return {};
        }

        Result<DependencyBinding> parse_dependency(const TableEntry& entry, const std::string& path) {
            SourceLocation location = entry.value.location();
            location.config_path = path;

            if (!is_valid_package_name(entry.key))
                return std::unexpected(error_at(
                    location,
                    "`" + entry.key + "` is not a valid dependency name"
                ));

            DependencyBinding dependency;
            dependency.request.package = entry.key;
            dependency.location = location;

            if (const std::string* shorthand = entry.value.as_string()) {
                auto version = parse_version_requirement(*shorthand, location);
                if (!version)
                    return std::unexpected(version.error());

                dependency.request.version = std::move(*version);
                return dependency;
            }

            auto table_result = TableReader::bind(entry.value, path);
            if (!table_result) {
                return std::unexpected(std::move(table_result).error().add_note(
                    "dependencies use a version string or a dependency table"
                ));
            }
            TableReader table = std::move(*table_result);

            auto version = table.optional_string("version");
            if (!version)
                return std::unexpected(version.error());
            if (*version) {
                auto requirement = parse_version_requirement(**version, table.location_of("version"));
                if (!requirement)
                    return std::unexpected(requirement.error());

                dependency.request.version = std::move(*requirement);
            }

            auto features = read_string_array(table, "features");
            if (!features)
                return std::unexpected(features.error());

            dependency.request.features = std::move(*features);

            auto optional = read_boolean(table, "optional");
            if (!optional)
                return std::unexpected(optional.error());

            dependency.request.optional = *optional;

            auto alias = table.optional_string("alias");
            if (!alias)
                return std::unexpected(alias.error());
            if (*alias) {
                if (!is_valid_identifier(**alias)) {
                    return std::unexpected(error_at(
                        table.location_of("alias"),
                        "`" + **alias + "` is not a valid dependency alias"
                    ));
                }
                dependency.alias = std::move(**alias);
            }

            auto provider = table.optional_string("from");
            if (!provider)
                return std::unexpected(provider.error());
            if (*provider) {
                if (!is_valid_identifier(**provider)) {
                    return std::unexpected(error_at(
                        table.location_of("from"),
                        "`" + **provider + "` is not a valid provider name"
                    ));
                }
                dependency.selection.provider = std::move(**provider);
            }

            auto directory = table.optional_string("path");
            if (!directory)
                return std::unexpected(directory.error());
            if (*directory) {
                if ((*directory)->empty())
                    return std::unexpected(error_at(table.location_of("path"), "path cannot be empty"));

                dependency.selection.path = std::filesystem::path(std::move(**directory));
            }

            constexpr std::array common_fields{
                std::string_view{"version"},
                std::string_view{"features"},
                std::string_view{"optional"},
                std::string_view{"alias"},
                std::string_view{"from"},
                std::string_view{"path"}
            };
            for (const TableEntry& field: table.entries()) {
                if (std::ranges::find(common_fields, field.key) != common_fields.end())
                    continue;

                table.take(field.key);
                if (!is_valid_identifier(field.key)) {
                    return std::unexpected(error_at(
                        field.value.location(),
                        "`" + field.key + "` is not a valid source driver name"
                    ));
                }
                if (!field.value.is_table()) {
                    return std::unexpected(error_at(
                        field.value.location(),
                        "source driver `" + field.key + "` options must be a table"
                    ));
                }
                if (dependency.selection.source) {
                    return std::unexpected(error_at(
                        field.value.location(),
                        "dependency selects more than one direct source"
                    ));
                }
                dependency.selection.source = SourceLocator{field.key, field.value};
            }

            if (dependency.selection.path && dependency.selection.source) {
                return std::unexpected(error_at(
                    location,
                    "dependency cannot combine `path` with a source driver"
                ));
            }
            if (dependency.selection.provider
                && (dependency.selection.path || dependency.selection.source)) {
                return std::unexpected(error_at(
                    location,
                    "dependency cannot combine `from` with a direct source"
                ));
            }

            auto finished = table.finish();
            if (!finished)
                return std::unexpected(finished.error());

            return dependency;
        }

        Result<PackageSet> parse_package_set(TableReader& table) {
            PackageSet package_set;
            package_set.location = table.location_of("members");

            auto members = read_string_array(table, "members");
            if (!members)
                return std::unexpected(members.error());
            if (members->empty()) {
                return std::unexpected(error_at(
                    table.location_of("members"),
                    "a package set requires at least one member pattern"
                ));
            }
            package_set.members = std::move(*members);

            auto exclude = read_string_array(table, "exclude");
            if (!exclude)
                return std::unexpected(exclude.error());

            package_set.exclude = std::move(*exclude);

            auto defaults = read_string_array(table, "default");
            if (!defaults)
                return std::unexpected(defaults.error());

            for (const std::string& name: *defaults) {
                if (!is_valid_package_name(name)) {
                    return std::unexpected(error_at(
                        table.location_of("default"),
                        "`" + name + "` is not a valid default package name"
                    ));
                }
            }
            package_set.defaults = std::move(*defaults);

            auto finished = table.finish();
            if (!finished)
                return std::unexpected(finished.error());

            return package_set;
        }
    }

    bool is_valid_identifier(const std::string_view name) noexcept {
        if (name.empty() || name.front() == '-')
            return false;

        return std::ranges::all_of(name, [](const char character) {
            return (character >= 'a' && character <= 'z')
                || (character >= 'A' && character <= 'Z')
                || (character >= '0' && character <= '9')
                || character == '_'
                || character == '-';
        });
    }

    bool is_valid_package_name(const std::string_view name) noexcept {
        if (name.empty() || name.front() == '-' || name.front() == '.')
            return false;

        return std::ranges::all_of(name, [](const char character) {
            return (character >= 'a' && character <= 'z')
                || (character >= 'A' && character <= 'Z')
                || (character >= '0' && character <= '9')
                || character == '_'
                || character == '-'
                || character == '.';
        });
    }

    bool is_valid_target_name(const std::string_view name) noexcept {
        if (name.empty() || name.front() == '-' || name.front() == '.')
            return false;

        return std::ranges::all_of(name, [](const char character) {
            return (character >= 'a' && character <= 'z')
                || (character >= 'A' && character <= 'Z')
                || (character >= '0' && character <= '9')
                || character == '_'
                || character == '-'
                || character == '.';
        });
    }

    Result<ManifestDocument> parse_manifest_document(const Value& document) {
        auto root_result = TableReader::bind(document);
        if (!root_result)
            return std::unexpected(root_result.error());

        TableReader root = std::move(*root_result);

        auto package_result = root.optional_table("package");
        if (!package_result)
            return std::unexpected(package_result.error());

        ManifestDocument result;
        if (*package_result) {
            TableReader package = std::move(**package_result);
            Manifest manifest;

            auto name = read_package_name(package, "name");
            if (!name)
                return std::unexpected(name.error());

            manifest.name = std::move(*name);
            manifest.location = package.location_of("name");

            auto version = package.optional_string("version");
            if (!version)
                return std::unexpected(version.error());
            if (*version) {
                auto parsed = parse_version(**version, package.location_of("version"));
                if (!parsed)
                    return std::unexpected(parsed.error());

                manifest.version = std::move(*parsed);
            }

            auto resolver = read_identifier(package, "resolver");
            if (!resolver)
                return std::unexpected(resolver.error());

            manifest.resolver = std::move(*resolver);

            auto tests = read_target_references(
                package,
                "tests",
                PackageTargetKind::test,
                manifest.target_references
            );
            if (!tests)
                return std::unexpected(tests.error());

            auto examples = read_target_references(
                package,
                "examples",
                PackageTargetKind::example,
                manifest.target_references
            );
            if (!examples)
                return std::unexpected(examples.error());

            auto benchmarks = read_target_references(
                package,
                "benchmarks",
                PackageTargetKind::benchmark,
                manifest.target_references
            );
            if (!benchmarks)
                return std::unexpected(benchmarks.error());

            auto package_finished = package.finish();
            if (!package_finished)
                return std::unexpected(package_finished.error());

            auto dependencies = read_dependencies(root);
            if (!dependencies)
                return std::unexpected(dependencies.error());

            manifest.dependencies = std::move(*dependencies);

            auto targets = parse_package_targets(root, manifest.resolver, manifest.targets);
            if (!targets)
                return std::unexpected(targets.error());

            if (const Value* options = root.take(manifest.resolver)) {
                if (!options->is_table()) {
                    return std::unexpected(error_at(
                        options->location(),
                        "resolver options must be a table"
                    ));
                }
                manifest.resolver_options = *options;
            }
            result.package = std::move(manifest);
        }

        auto package_set_result = root.optional_table("package-set");
        if (!package_set_result)
            return std::unexpected(package_set_result.error());
        if (*package_set_result) {
            TableReader package_set = std::move(**package_set_result);
            auto parsed = parse_package_set(package_set);
            if (!parsed)
                return std::unexpected(parsed.error());

            result.package_set = std::move(*parsed);
        }

        if (!result.package && !result.package_set) {
            return std::unexpected(error_at(
                document.location(),
                "manifest requires a `[package]` or `[package-set]` table"
            ));
        }

        auto configurations = read_configuration_set(root);
        if (!configurations)
            return std::unexpected(configurations.error());

        result.configurations = std::move(*configurations);
        if (result.package)
            result.package->configurations = result.configurations;

        auto providers = read_provider_definitions(root);
        if (!providers)
            return std::unexpected(providers.error());

        result.providers = std::move(*providers);

        auto root_finished = root.finish();
        if (!root_finished)
            return std::unexpected(root_finished.error());

        return result;
    }

    Result<ManifestDocument> parse_manifest_document_file(const std::filesystem::path& path) {
        auto document = parse_file(path);
        if (!document)
            return std::unexpected(document.error());

        auto manifest = parse_manifest_document(*document);
        if (!manifest)
            return std::unexpected(manifest.error());

        manifest->source = path;
        if (manifest->package) {
            manifest->package->source = path;
            for (PackageTarget& target: manifest->package->targets)
                target.source = path;
        }
        return manifest;
    }

    Result<ManifestDocument> parse_manifest_document_string(
        const std::string_view text,
        const std::string_view source_name
    ) {
        auto document = parse_string(text, source_name);
        if (!document)
            return std::unexpected(document.error());

        auto manifest = parse_manifest_document(*document);
        if (!manifest)
            return std::unexpected(manifest.error());

        manifest->source = std::filesystem::path(source_name);
        if (manifest->package) {
            manifest->package->source = manifest->source;
            for (PackageTarget& target: manifest->package->targets)
                target.source = manifest->source;
        }
        return manifest;
    }

    Result<Manifest> parse_manifest(const Value& document) {
        auto parsed = parse_manifest_document(document);
        if (!parsed)
            return std::unexpected(parsed.error());
        if (!parsed->package)
            return std::unexpected(error_at(document.location(), "manifest does not declare a package"));

        return std::move(*parsed->package);
    }

    Result<Manifest> parse_manifest_file(const std::filesystem::path& path) {
        auto document = parse_manifest_document_file(path);
        if (!document)
            return std::unexpected(document.error());
        if (!document->package)
            return std::unexpected(error_at({}, "manifest `" + path.string() + "` does not declare a package"));

        return std::move(*document->package);
    }

    Result<Manifest> parse_manifest_string(const std::string_view text, const std::string_view source_name) {
        auto document = parse_manifest_document_string(text, source_name);
        if (!document)
            return std::unexpected(document.error());
        if (!document->package) {
            return std::unexpected(error_at(
                {},
                "manifest `" + std::string(source_name) + "` does not declare a package"
            ));
        }

        return std::move(*document->package);
    }

    Result<std::vector<PackageTarget>> parse_package_targets_file(const std::filesystem::path& path, const PackageTargetKind kind, const std::string_view resolver) {
        auto document = parse_file(path);
        if (!document)
            return std::unexpected(document.error());

        auto root_result = TableReader::bind(*document);
        if (!root_result)
            return std::unexpected(root_result.error());

        TableReader root = std::move(*root_result);

        std::vector<PackageTarget> targets;
        auto parsed = parse_package_targets(root, resolver, targets, kind);
        if (!parsed)
            return std::unexpected(parsed.error());

        if (targets.empty()) {
            return std::unexpected(error_at(
                document->location(),
                "referenced target manifest declares no targets"
            ));
        }

        auto finished = root.finish();
        if (!finished)
            return std::unexpected(finished.error());

        for (PackageTarget& target: targets)
            target.source = path;

        return targets;
    }
}
