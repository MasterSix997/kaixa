#include <kaixa/model/manifest.hpp>

#include <kaixa/config/parser.hpp>
#include <kaixa/config/table_reader.hpp>

#include <algorithm>
#include <array>
#include <set>
#include <tuple>
#include <utility>

namespace kaixa {
    namespace {
        Result<DependencyBinding> parse_dependency(const TableEntry& entry, const std::string& path);

        bool is_valid_target_template(std::string value) {
            std::size_t position = 0;
            while ((position = value.find('{', position)) != std::string::npos) {
                const std::size_t end = value.find('}', position + 1);
                if (end == std::string::npos || !is_valid_identifier(std::string_view(value).substr(position + 1, end - position - 1)))
                    return false;

                value.replace(position, end - position + 1, "capture");
                position += std::string_view("capture").size();
            }
            return is_valid_target_name(value);
        }

        Result<std::string> read_package_name(TableReader& table, const std::string_view key) {
            auto value = table.string(key);
            if (!value)
                return std::unexpected(value.error());

            if (!is_valid_package_name(*value)) {
                return std::unexpected(error_at(table.location_of(key), "`" + *value + "` is not a valid package name"));
            }
            return *value;
        }

        Result<std::vector<std::string>> read_string_array_value(const Value& value, const std::string_view description) {
            const std::vector<Value>* array = value.as_array();
            if (!array) {
                return std::unexpected(
                    error_at(value.location(), "expected an array, found " + std::string(value_kind_name(value.kind())))
                );
            }

            std::vector<std::string> result;
            result.reserve(array->size());
            for (const Value& item: *array) {
                const std::string* text = item.as_string();
                if (!text)
                    return std::unexpected(error_at(item.location(), "expected a string in " + std::string(description)));

                if (text->empty())
                    return std::unexpected(error_at(item.location(), "array values cannot be empty"));

                result.push_back(*text);
            }

            return result;
        }

        Result<TaskDeclaration> parse_task_declaration(
            const Value& value,
            const std::string& path,
            const std::string& name,
            const bool allow_package_scope
        ) {
            auto table_result = TableReader::bind(value, path);
            if (!table_result)
                return std::unexpected(table_result.error());

            TableReader table = std::move(*table_result);
            TaskDeclaration result;
            result.name = name;
            result.location = value.location();
            result.source = result.location.source;

            if (allow_package_scope) {
                auto package = table.optional_string("package");
                if (!package)
                    return std::unexpected(package.error());

                if (*package) {
                    if (!is_valid_package_name(**package)) {
                        return std::unexpected(error_at(table.location_of("package"), "`" + **package + "` is not a valid package name"));
                    }
                    result.package = std::move(**package);
                }
            }

            const Value* run = table.take("run");
            if (run) {
                auto arguments = read_string_array_value(*run, "command run");
                if (!arguments)
                    return std::unexpected(arguments.error());

                result.run = std::move(*arguments);
            }

            auto tool = table.optional_string("tool");
            if (!tool)
                return std::unexpected(tool.error());

            auto script = table.optional_string("script");
            if (!script)
                return std::unexpected(script.error());

            if (run && (*tool || *script)) {
                return std::unexpected(
                    error_at(value.location(), "command `" + result.name + "` cannot combine `run` with `tool` or `script`")
                );
            }
            if (run) {
                if (result.run.empty())
                    return std::unexpected(error_at(run->location(), "command `run` cannot be empty"));

            } else {
                if (!*tool || !*script) {
                    return std::unexpected(
                        error_at(value.location(), "command `" + result.name + "` requires either `run` or both `tool` and `script`")
                    );
                }
                if ((*tool)->empty() || (*script)->empty()) {
                    return std::unexpected(error_at(value.location(), "command `tool` and `script` cannot be empty"));
                }
                result.run = {std::move(**tool), std::move(**script)};
            }

            auto working_directory = table.optional_string("working-directory");
            if (!working_directory)
                return std::unexpected(working_directory.error());

            if (*working_directory) {
                if ((*working_directory)->empty()) {
                    return std::unexpected(error_at(table.location_of("working-directory"), "command working directory cannot be empty"));
                }
                result.working_directory = std::move(**working_directory);
            }

            auto environment_result = table.optional_table("environment");
            if (!environment_result)
                return std::unexpected(environment_result.error());

            if (*environment_result) {
                TableReader environment = std::move(**environment_result);
                for (const TableEntry& entry: environment.entries()) {
                    if (entry.key.empty() || entry.key.contains('=')) {
                        return std::unexpected(
                            error_at(entry.value.location(), "invalid command environment variable name `" + entry.key + "`")
                        );
                    }
                    const std::string* environment_value = entry.value.as_string();
                    if (!environment_value) {
                        return std::unexpected(error_at(entry.value.location(), "command environment values must be strings"));
                    }
                    result.environment.emplace(entry.key, *environment_value);
                }
                environment.take_all();
            }

            auto inputs = table.string_array("inputs");
            if (!inputs)
                return std::unexpected(inputs.error());

            for (std::string& input: *inputs)
                result.inputs.emplace_back(std::move(input));

            auto outputs = table.string_array("outputs");
            if (!outputs)
                return std::unexpected(outputs.error());

            for (std::string& output: *outputs)
                result.outputs.emplace_back(std::move(output));

            auto after = table.string_array("after");
            if (!after)
                return std::unexpected(after.error());

            result.after = std::move(*after);
            auto finished = table.finish();
            if (!finished)
                return std::unexpected(finished.error());

            return result;
        }

        Result<std::vector<TaskDeclaration>> read_task_declarations(TableReader& table, const bool allow_package_scope = false) {
            auto commands_result = table.optional_table("command");
            if (!commands_result)
                return std::unexpected(commands_result.error());

            if (!*commands_result)
                return std::vector<TaskDeclaration>{};

            TableReader commands = std::move(**commands_result);
            std::vector<TaskDeclaration> result;
            result.reserve(commands.entries().size());
            for (const TableEntry& entry: commands.entries()) {
                if (!is_valid_target_name(entry.key)) {
                    return std::unexpected(error_at(entry.value.location(), "`" + entry.key + "` is not a valid command name"));
                }

                auto command = parse_task_declaration(
                    entry.value,
                    join_config_path(commands.path(), entry.key),
                    entry.key,
                    allow_package_scope
                );
                if (!command)
                    return std::unexpected(command.error());

                result.push_back(std::move(*command));
            }
            commands.take_all();
            return result;
        }

        Result<std::vector<WorkflowDeclaration>> read_workflow_declarations(TableReader& table, const bool allow_package_scope = false) {
            auto workflows_result = table.optional_table("workflow");
            if (!workflows_result)
                return std::unexpected(workflows_result.error());

            if (!*workflows_result)
                return std::vector<WorkflowDeclaration>{};

            TableReader workflows = std::move(**workflows_result);
            std::vector<WorkflowDeclaration> result;
            result.reserve(workflows.entries().size());
            for (const TableEntry& entry: workflows.entries()) {
                if (!is_valid_target_name(entry.key)) {
                    return std::unexpected(error_at(entry.value.location(), "`" + entry.key + "` is not a valid workflow name"));
                }

                auto declaration_result = TableReader::bind(entry.value, join_config_path(workflows.path(), entry.key));
                if (!declaration_result)
                    return std::unexpected(declaration_result.error());

                TableReader declaration = std::move(*declaration_result);
                std::optional<std::string> package;
                if (allow_package_scope) {
                    auto selected_package = declaration.optional_string("package");
                    if (!selected_package)
                        return std::unexpected(selected_package.error());

                    if (*selected_package) {
                        if (!is_valid_package_name(**selected_package)) {
                            return std::unexpected(
                                error_at(declaration.location_of("package"), "`" + **selected_package + "` is not a valid package name")
                            );
                        }
                        package = std::move(**selected_package);
                    }
                }

                auto steps = declaration.string_array("steps");
                if (!steps)
                    return std::unexpected(steps.error());

                if (steps->empty()) {
                    return std::unexpected(error_at(entry.value.location(), "workflow `" + entry.key + "` requires at least one step"));
                }

                auto finished = declaration.finish();
                if (!finished)
                    return std::unexpected(finished.error());

                result.push_back({entry.key, std::move(package), std::move(*steps), entry.value.location().source, entry.value.location()});
            }
            workflows.take_all();
            return result;
        }

        Result<std::vector<DependencyBinding>> read_dependencies(
            TableReader& table,
            const std::string_view key = "dependencies",
            const DependencyVisibility visibility = DependencyVisibility::private_dependency
        ) {
            auto dependencies_result = table.optional_table(key);
            if (!dependencies_result)
                return std::unexpected(dependencies_result.error());

            std::vector<DependencyBinding> result;
            if (!*dependencies_result)
                return result;

            TableReader dependencies = std::move(**dependencies_result);
            result.reserve(dependencies.entries().size());
            for (const TableEntry& entry: dependencies.entries()) {
                auto dependency = parse_dependency(entry, join_config_path(dependencies.path(), entry.key));
                if (!dependency)
                    return std::unexpected(dependency.error());

                dependency->visibility = visibility;
                result.push_back(std::move(*dependency));
            }
            dependencies.take_all();
            return result;
        }

        Result<std::vector<FeatureDefinition>> read_features(TableReader& root, std::vector<std::string>& defaults) {
            auto features_result = root.optional_table("features");
            if (!features_result)
                return std::unexpected(features_result.error());

            if (!*features_result)
                return std::vector<FeatureDefinition>{};

            TableReader features = std::move(**features_result);
            std::vector<FeatureDefinition> result;
            for (const TableEntry& entry: features.entries()) {
                if (!is_valid_identifier(entry.key)) {
                    return std::unexpected(error_at(entry.value.location(), "`" + entry.key + "` is not a valid feature name"));
                }

                if (entry.key == "default") {
                    auto values = read_string_array_value(entry.value, "default features");
                    if (!values)
                        return std::unexpected(values.error());

                    defaults = std::move(*values);
                    features.take(entry.key);
                    continue;
                }

                FeatureDefinition definition;
                definition.name = entry.key;
                definition.location = entry.value.location();
                if (entry.value.as_array()) {
                    definition.legacy = true;
                    auto values = read_string_array_value(entry.value, "feature dependencies");
                    if (!values)
                        return std::unexpected(values.error());

                    definition.features = std::move(*values);
                    features.take(entry.key);
                    result.push_back(std::move(definition));
                    continue;
                }

                auto definition_result = TableReader::bind(entry.value, join_config_path(features.path(), entry.key));
                if (!definition_result)
                    return std::unexpected(definition_result.error());

                TableReader feature = std::move(*definition_result);

                auto local_features = feature.string_array("features");
                if (!local_features)
                    return std::unexpected(local_features.error());

                definition.features = std::move(*local_features);

                auto dependencies = feature.string_array("dependencies");
                if (!dependencies)
                    return std::unexpected(dependencies.error());

                definition.dependencies = std::move(*dependencies);

                auto members = feature.string_array("members");
                if (!members)
                    return std::unexpected(members.error());

                definition.members = std::move(*members);

                auto dependency_features_result = feature.optional_table("dependency-features");
                if (!dependency_features_result)
                    return std::unexpected(dependency_features_result.error());

                if (*dependency_features_result) {
                    TableReader dependency_features = std::move(**dependency_features_result);
                    for (const TableEntry& dependency: dependency_features.entries()) {
                        auto values = read_string_array_value(dependency.value, "dependency features");
                        if (!values)
                            return std::unexpected(values.error());

                        definition.dependency_features.emplace(dependency.key, std::move(*values));
                    }
                    dependency_features.take_all();
                }

                auto finished = feature.finish();
                if (!finished)
                    return std::unexpected(finished.error());

                features.take(entry.key);
                result.push_back(std::move(definition));
            }
            features.take_all();
            return result;
        }

        Result<void> read_target_references(
            TableReader& package,
            const std::string_view key,
            const PackageTargetKind kind,
            std::vector<PackageTargetReference>& output
        ) {
            auto paths = package.string_array(key);
            if (!paths)
                return std::unexpected(paths.error());

            for (std::string& path: *paths) {
                output.push_back({kind, std::filesystem::path(path), package.location_of(key)});
            }

            return {};
        }

        Result<void> read_target_identity(TableReader& table, PackageTarget& target) {
            auto name = table.optional_string("name");
            if (!name)
                return std::unexpected(name.error());

            if (*name) {
                if (!is_valid_target_name(**name) && !is_valid_target_template(**name)) {
                    return std::unexpected(error_at(table.location_of("name"), "`" + **name + "` is not a valid target name"));
                }
                target.name = std::move(**name);
            }

            auto name_template = table.optional_string("name-template");
            if (!name_template)
                return std::unexpected(name_template.error());

            if (*name_template) {
                if (target.name) {
                    return std::unexpected(
                        error_at(table.location_of("name-template"), "a target cannot combine `name` and `name-template`")
                    );
                }
                if (!is_valid_target_template(**name_template)) {
                    return std::unexpected(
                        error_at(table.location_of("name-template"), "`" + **name_template + "` is not a valid target name template")
                    );
                }
                target.name = std::move(**name_template);
                target.name_template = true;
            }

            return {};
        }

        Result<void> read_target_metadata(TableReader& table, PackageTarget& target) {
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

            return {};
        }

        Result<void> read_target_sources(TableReader& table, PackageTarget& target, const bool allow_partial) {
            auto sources = table.string_array("sources");
            if (!sources)
                return std::unexpected(sources.error());

            auto source = table.optional_string("source");
            if (!source)
                return std::unexpected(source.error());

            if (*source) {
                if (!sources->empty()) {
                    return std::unexpected(error_at(table.location_of("source"), "a target cannot combine `source` and `sources`"));
                }
                sources->push_back(std::move(**source));
            }

            if (sources->empty()) {
                if (allow_partial) {
                    target.partial = true;
                } else {
                    return std::unexpected(error_at(table.location_of("sources"), "a package target requires at least one source pattern"));
                }
            }
            target.sources.include = std::move(*sources);
            target.sources.location = table.location_of("sources");

            auto excludes = table.string_array("source-excludes");
            if (!excludes)
                return std::unexpected(excludes.error());

            auto declarative_excludes = table.string_array("exclude");
            if (!declarative_excludes)
                return std::unexpected(declarative_excludes.error());

            excludes->insert(
                excludes->end(),
                std::make_move_iterator(declarative_excludes->begin()),
                std::make_move_iterator(declarative_excludes->end())
            );

            target.sources.exclude = std::move(*excludes);

            return {};
        }

        Result<void> read_target_required_features(TableReader& table, PackageTarget& target) {
            if (const Value* required_features = table.take("required-features")) {
                if (required_features->as_array()) {
                    auto values = read_string_array_value(*required_features, "required-features");
                    if (!values)
                        return std::unexpected(values.error());

                    target.required_features = std::move(*values);
                } else if (const std::vector<TableEntry>* requirements = required_features->as_table()) {
                    for (const TableEntry& requirement: *requirements) {
                        if (!is_valid_package_name(requirement.key)) {
                            return std::unexpected(
                                error_at(requirement.value.location(), "`" + requirement.key + "` is not a valid required package name")
                            );
                        }
                        auto values = read_string_array_value(requirement.value, "required-features." + requirement.key);
                        if (!values)
                            return std::unexpected(values.error());

                        target.required_dependency_features.emplace(requirement.key, std::move(*values));
                    }
                } else {
                    return std::unexpected(
                        error_at(required_features->location(), "required-features must be an array or a package-to-features table")
                    );
                }
            }

            return {};
        }

        Result<void> read_target_behavior(TableReader& table, PackageTarget& target) {
            auto dependencies = read_dependencies(table);
            if (!dependencies)
                return std::unexpected(dependencies.error());

            target.dependencies = std::move(*dependencies);

            auto arguments = table.string_array("arguments");
            if (!arguments)
                return std::unexpected(arguments.error());

            target.arguments = std::move(*arguments);

            auto discover = table.boolean("discover");
            if (!discover)
                return std::unexpected(discover.error());

            target.discover = *discover;

            auto hidden = table.boolean("hidden");
            if (!hidden)
                return std::unexpected(hidden.error());

            target.hidden = *hidden;

            auto install = table.boolean("install");
            if (!install)
                return std::unexpected(install.error());

            target.install = *install;

            auto framework = table.optional_string("framework");
            if (!framework)
                return std::unexpected(framework.error());

            target.framework = std::move(*framework);

            if (const Value* policy = table.take("policy")) {
                if (!policy->is_table()) {
                    return std::unexpected(error_at(policy->location(), "target policy must be a table"));
                }
                target.policy = *policy;
            }

            if (const Value* matrix = table.take("matrix")) {
                const std::vector<TableEntry>* axes = matrix->as_table();
                if (!axes) {
                    return std::unexpected(error_at(matrix->location(), "target matrix must be a table"));
                }
                TargetMatrix parsed;
                parsed.location = matrix->location();
                for (const TableEntry& axis: *axes) {
                    if (!is_valid_identifier(axis.key)) {
                        return std::unexpected(error_at(axis.value.location(), "invalid target matrix axis `" + axis.key + "`"));
                    }
                    const std::vector<Value>* values = axis.value.as_array();
                    if (!values || values->empty()) {
                        return std::unexpected(
                            error_at(axis.value.location(), "target matrix axis `" + axis.key + "` must be a non-empty array")
                        );
                    }
                    for (const Value& value: *values) {
                        if (value.kind() == ValueKind::none || value.kind() == ValueKind::array || value.kind() == ValueKind::table) {
                            return std::unexpected(error_at(value.location(), "target matrix values must be booleans, numbers or strings"));
                        }
                    }
                    parsed.axes.push_back({axis.key, *values, axis.value.location()});
                }
                if (parsed.axes.empty())
                    return std::unexpected(error_at(matrix->location(), "target matrix must declare at least one axis"));

                target.matrix = std::move(parsed);
            }

            return {};
        }

        Result<void> read_target_resolver_options(TableReader& table, PackageTarget& target, const std::string_view resolver) {
            if (!resolver.empty()) {
                if (const Value* options = table.take(resolver)) {
                    if (!options->is_table()) {
                        return std::unexpected(error_at(options->location(), "target resolver options must be a table"));
                    }
                    target.resolver_options = *options;
                }
            }

            Value remaining = table.take_remaining();
            const std::vector<TableEntry>* leftover = remaining.as_table();
            if (leftover && !leftover->empty()) {
                std::vector<TableEntry> direct_options(leftover->begin(), leftover->end());
                if (target.resolver_options) {
                    const std::vector<TableEntry>* existing = target.resolver_options->as_table();
                    direct_options.insert(direct_options.begin(), existing->begin(), existing->end());
                }
                target.resolver_options = Value::table(std::move(direct_options), table.location_of("sources"));
            }

            return {};
        }

        Result<PackageTarget> parse_package_target(
            TableReader& table,
            const PackageTargetKind kind,
            const bool each_source,
            const std::string_view resolver,
            const bool allow_partial
        ) {
            PackageTarget target;
            target.kind = kind;
            target.each_source = each_source;
            target.location = table.location_of("sources");

            auto identity = read_target_identity(table, target);
            if (!identity)
                return std::unexpected(identity.error());

            auto metadata = read_target_metadata(table, target);
            if (!metadata)
                return std::unexpected(metadata.error());

            auto sources = read_target_sources(table, target, allow_partial);
            if (!sources)
                return std::unexpected(sources.error());

            auto required_features = read_target_required_features(table, target);
            if (!required_features)
                return std::unexpected(required_features.error());

            auto behavior = read_target_behavior(table, target);
            if (!behavior)
                return std::unexpected(behavior.error());

            auto resolver_options = read_target_resolver_options(table, target, resolver);
            if (!resolver_options)
                return std::unexpected(resolver_options.error());

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
            std::vector<PackageTarget>& output,
            const bool allow_partial = false
        ) {
            const Value* value = root.take(key);
            if (!value)
                return {};

            if (value->is_table()) {
                auto table_result = TableReader::bind(*value, std::string(key));
                if (!table_result)
                    return std::unexpected(table_result.error());

                TableReader table = std::move(*table_result);
                auto target = parse_package_target(table, kind, each_source, resolver, allow_partial);
                if (!target)
                    return std::unexpected(target.error());

                output.push_back(std::move(*target));
                return {};
            }

            const std::vector<Value>* array = value->as_array();
            if (!array) {
                return std::unexpected(error_at(root.location_of(key), "expected a target table or array of target tables"));
            }

            for (std::size_t index = 0; index < array->size(); ++index) {
                auto table_result = TableReader::bind((*array)[index], std::string(key) + "." + std::to_string(index));
                if (!table_result)
                    return std::unexpected(table_result.error());

                TableReader table = std::move(*table_result);
                auto target = parse_package_target(table, kind, each_source, resolver, allow_partial);
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
            const std::optional<PackageTargetKind> only = std::nullopt,
            const bool allow_partial = false
        ) {
            struct TargetSection {
                std::string_view singular;
                std::string_view plural;
                PackageTargetKind kind;
            };
            constexpr std::array sections{TargetSection{"test", "tests", PackageTargetKind::test},
                TargetSection{"example", "examples", PackageTargetKind::example},
                TargetSection{"benchmark", "benchmarks", PackageTargetKind::benchmark}};

            for (const TargetSection& section: sections) {
                if (only && section.kind != *only)
                    continue;

                auto singular = parse_target_collection(root, section.singular, section.kind, false, resolver, output, allow_partial);
                if (!singular)
                    return std::unexpected(singular.error());

                auto plural = parse_target_collection(root, section.plural, section.kind, true, resolver, output, allow_partial);
                if (!plural)
                    return std::unexpected(plural.error());
            }

            return {};
        }

        Result<DependencyBinding> parse_dependency(const TableEntry& entry, const std::string& path) {
            SourceLocation location = entry.value.location();
            location.config_path = path;

            if (!is_valid_package_name(entry.key))
                return std::unexpected(error_at(location, "`" + entry.key + "` is not a valid dependency name"));

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
                return std::unexpected(std::move(table_result).error().add_note("dependencies use a version string or a dependency table"));
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

            auto features = table.string_array("features");
            if (!features)
                return std::unexpected(features.error());

            dependency.request.features = std::move(*features);

            auto optional = table.boolean("optional");
            if (!optional)
                return std::unexpected(optional.error());

            dependency.request.optional = *optional;

            auto alias = table.optional_string("alias");
            if (!alias)
                return std::unexpected(alias.error());

            if (*alias) {
                if (!is_valid_identifier(**alias)) {
                    return std::unexpected(error_at(table.location_of("alias"), "`" + **alias + "` is not a valid dependency alias"));
                }
                dependency.alias = std::move(**alias);
            }

            auto provider = table.optional_string("from");
            if (!provider)
                return std::unexpected(provider.error());

            if (*provider) {
                if (!is_valid_identifier(**provider)) {
                    return std::unexpected(error_at(table.location_of("from"), "`" + **provider + "` is not a valid provider name"));
                }
            }

            auto directory = table.optional_string("path");
            if (!directory)
                return std::unexpected(directory.error());

            if (*directory) {
                if ((*directory)->empty())
                    return std::unexpected(error_at(table.location_of("path"), "path cannot be empty"));
            }

            std::optional<SourceLocator> source;

            constexpr std::array common_fields{std::string_view{"version"},
                std::string_view{"features"},
                std::string_view{"optional"},
                std::string_view{"alias"},
                std::string_view{"from"},
                std::string_view{"path"}};
            for (const TableEntry& field: table.entries()) {
                if (std::ranges::find(common_fields, field.key) != common_fields.end())
                    continue;

                table.take(field.key);
                if (!is_valid_identifier(field.key)) {
                    return std::unexpected(error_at(field.value.location(), "`" + field.key + "` is not a valid source driver name"));
                }
                if (!field.value.is_table()) {
                    return std::unexpected(error_at(field.value.location(), "source driver `" + field.key + "` options must be a table"));
                }
                if (source) {
                    return std::unexpected(error_at(field.value.location(), "dependency selects more than one direct source"));
                }
                source = SourceLocator{field.key, field.value};
            }

            if (*directory && source) {
                return std::unexpected(error_at(location, "dependency cannot combine `path` with a source driver"));
            }
            if (*provider && (*directory || source)) {
                return std::unexpected(error_at(location, "dependency cannot combine `from` with a direct source"));
            }

            if (*provider)
                dependency.selection = CandidateSelection::from_provider(std::move(**provider));
            else if (*directory)
                dependency.selection = CandidateSelection::from_path(std::filesystem::path(**directory));
            else if (source)
                dependency.selection = CandidateSelection::from_source(std::move(*source));

            auto finished = table.finish();
            if (!finished)
                return std::unexpected(finished.error());

            return dependency;
        }

        Result<PackageSet> parse_package_set(TableReader& table) {
            PackageSet package_set;
            package_set.location = table.location_of("members");

            auto name = table.optional_string("name");
            if (!name)
                return std::unexpected(name.error());

            if (*name) {
                if (!is_valid_package_name(**name)) {
                    return std::unexpected(error_at(table.location_of("name"), "`" + **name + "` is not a valid package set name"));
                }
                package_set.name = std::move(**name);
            }

            auto members = table.string_array("members");
            if (!members)
                return std::unexpected(members.error());

            package_set.members = std::move(*members);

            auto exclude = table.string_array("exclude");
            if (!exclude)
                return std::unexpected(exclude.error());

            package_set.exclude = std::move(*exclude);

            auto defaults = table.string_array("default");
            if (!defaults)
                return std::unexpected(defaults.error());

            for (const std::string& default_name: *defaults) {
                if (!is_valid_package_name(default_name)) {
                    return std::unexpected(
                        error_at(table.location_of("default"), "`" + default_name + "` is not a valid default package name")
                    );
                }
            }
            package_set.defaults = std::move(*defaults);
            package_set.defaults_location = table.location_of("default");

            if (const Value* policy = table.take("policy")) {
                if (!policy->is_table()) {
                    return std::unexpected(error_at(policy->location(), "package set policy must be a table"));
                }
                package_set.policy = *policy;
            }

            auto provider_config = table.optional_string("provider-config");
            if (!provider_config)
                return std::unexpected(provider_config.error());

            if (*provider_config) {
                if ((*provider_config)->empty()) {
                    return std::unexpected(error_at(table.location_of("provider-config"), "provider config path cannot be empty"));
                }
                package_set.provider_config = std::filesystem::path(**provider_config);
            }

            auto finished = table.finish();
            if (!finished)
                return std::unexpected(finished.error());

            return package_set;
        }

        Result<void> read_products(TableReader& root, Manifest& manifest) {
            for (
                const auto& [key, kind]: {std::pair{std::string_view{"lib"}, ProductDeclarationKind::library},
                    std::pair{std::string_view{"bin"}, ProductDeclarationKind::executable}}
            ) {
                const Value* product = root.take(key);
                if (!product)
                    continue;

                if (!product->is_table()) {
                    return std::unexpected(error_at(product->location(), "product `" + std::string(key) + "` must be a table"));
                }
                manifest.products.push_back({kind, *product, product->location()});
            }
            return {};
        }

        Result<Manifest> parse_inline_member(const TableEntry& entry, const std::string& path) {
            if (!is_valid_package_name(entry.key)) {
                return std::unexpected(error_at(entry.value.location(), "`" + entry.key + "` is not a valid inline package name"));
            }

            auto member_result = TableReader::bind(entry.value, path);
            if (!member_result)
                return std::unexpected(member_result.error());

            TableReader member = std::move(*member_result);

            Manifest manifest;
            manifest.name = entry.key;
            manifest.location = entry.value.location();

            auto version = member.optional_string("version");
            if (!version)
                return std::unexpected(version.error());

            if (*version) {
                auto parsed = parse_version(**version, member.location_of("version"));
                if (!parsed)
                    return std::unexpected(parsed.error());

                manifest.version = std::move(*parsed);
            }

            auto resolver = member.optional_string("resolver");
            if (!resolver)
                return std::unexpected(resolver.error());

            if (*resolver) {
                if (!is_valid_identifier(**resolver)) {
                    return std::unexpected(error_at(member.location_of("resolver"), "`" + **resolver + "` is not a valid resolver name"));
                }
                manifest.resolver = std::move(**resolver);
            }

            auto dependencies = read_dependencies(member);
            if (!dependencies)
                return std::unexpected(dependencies.error());

            manifest.dependencies = std::move(*dependencies);

            auto public_dependencies = read_dependencies(member, "public-dependencies", DependencyVisibility::public_dependency);
            if (!public_dependencies)
                return std::unexpected(public_dependencies.error());

            manifest.dependencies.insert(
                manifest.dependencies.end(),
                std::make_move_iterator(public_dependencies->begin()),
                std::make_move_iterator(public_dependencies->end())
            );

            auto features = read_features(member, manifest.default_features);
            if (!features)
                return std::unexpected(features.error());

            manifest.features = std::move(*features);

            auto products = read_products(member, manifest);
            if (!products)
                return std::unexpected(products.error());

            auto commands = read_task_declarations(member);
            if (!commands)
                return std::unexpected(commands.error());

            manifest.commands = std::move(*commands);

            auto workflows = read_workflow_declarations(member);
            if (!workflows)
                return std::unexpected(workflows.error());

            manifest.workflows = std::move(*workflows);

            if (!manifest.resolver.empty()) {
                if (const Value* options = member.take(manifest.resolver)) {
                    if (!options->is_table()) {
                        return std::unexpected(error_at(options->location(), "resolver options must be a table"));
                    }
                    manifest.resolver_options = *options;
                }
            }

            auto finished = member.finish();
            if (!finished)
                return std::unexpected(finished.error());

            if (manifest.resolver.empty() && !manifest.products.empty()) {
                return std::unexpected(
                    error_at(manifest.location, "inline package `" + manifest.name + "` declares a product without a resolver")
                );
            }
            return manifest;
        }

        Result<void> append_document_fragment(
            ManifestDocument& document,
            const std::filesystem::path& path,
            std::set<std::filesystem::path>& loading
        ) {
            std::error_code failure;
            const std::filesystem::path canonical = std::filesystem::canonical(path, failure);
            if (failure) {
                return std::unexpected(error("cannot resolve imported document `" + path.string() + "`: " + failure.message()));
            }
            if (!loading.insert(canonical).second) {
                return std::unexpected(error("document import cycle reaches `" + canonical.string() + "`"));
            }

            auto value = parse_file(canonical);
            if (!value)
                return std::unexpected(value.error());

            auto root_result = TableReader::bind(*value);
            if (!root_result)
                return std::unexpected(root_result.error());

            TableReader root = std::move(*root_result);

            if (root.take("package") || root.take("package-set") || root.take("members")) {
                return std::unexpected(error_at(value->location(), "imported document cannot declare packages or package sets"));
            }

            auto imports = root.string_array("imports");
            if (!imports)
                return std::unexpected(imports.error());

            for (const std::string& import: *imports) {
                auto appended = append_document_fragment(document, canonical.parent_path() / std::filesystem::path(import), loading);
                if (!appended)
                    return std::unexpected(appended.error());
            }

            auto providers = read_provider_definitions(root);
            if (!providers)
                return std::unexpected(providers.error());

            for (ProviderDefinition& provider: *providers) {
                if (std::ranges::any_of(document.providers, [&](const ProviderDefinition& existing) {
                        return existing.name == provider.name;
                    })) {
                    return std::unexpected(error_at(provider.location, "provider `" + provider.name + "` is declared more than once"));
                }
                provider.directory = canonical.parent_path();
                document.providers.push_back(std::move(provider));
            }

            auto routing_result = root.optional_table("routing");
            if (!routing_result)
                return std::unexpected(routing_result.error());

            if (*routing_result) {
                TableReader routing = std::move(**routing_result);
                for (const TableEntry& entry: routing.entries()) {
                    const std::string* provider = entry.value.as_string();
                    if (!provider) {
                        return std::unexpected(error_at(entry.value.location(), "provider routing values must be strings"));
                    }
                    if (document.routing.contains(entry.key)) {
                        return std::unexpected(
                            error_at(entry.value.location(), "provider route `" + entry.key + "` is declared more than once")
                        );
                    }
                    document.routing.emplace(entry.key, *provider);
                }
                routing.take_all();
            }

            auto configurations = read_configuration_set(root);
            if (!configurations)
                return std::unexpected(configurations.error());

            document.configurations.defaults
                .insert(document.configurations.defaults.end(), configurations->defaults.begin(), configurations->defaults.end());
            document.configurations.definitions.insert(
                document.configurations.definitions.end(),
                std::make_move_iterator(configurations->definitions.begin()),
                std::make_move_iterator(configurations->definitions.end())
            );

            auto commands = read_task_declarations(root);
            if (!commands)
                return std::unexpected(commands.error());

            if (!commands->empty()) {
                if (!document.package) {
                    return std::unexpected(error_at(commands->front().location, "commands require a package in the importing manifest"));
                }
                document.package->commands.insert(
                    document.package->commands.end(),
                    std::make_move_iterator(commands->begin()),
                    std::make_move_iterator(commands->end())
                );
            }

            auto workflows = read_workflow_declarations(root);
            if (!workflows)
                return std::unexpected(workflows.error());

            if (!workflows->empty()) {
                if (!document.package) {
                    return std::unexpected(error_at(workflows->front().location, "workflows require a package in the importing manifest"));
                }
                for (WorkflowDeclaration& workflow: *workflows) {
                    if (std::ranges::any_of(document.package->workflows, [&](const WorkflowDeclaration& existing) {
                            return existing.name == workflow.name;
                        })) {
                        return std::unexpected(error_at(workflow.location, "workflow `" + workflow.name + "` is declared more than once"));
                    }
                    document.package->workflows.push_back(std::move(workflow));
                }
            }

            auto finished = root.finish();
            if (!finished)
                return std::unexpected(finished.error());

            loading.erase(canonical);
            return {};
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

    Result<AutomationDocument> read_automation_document(TableReader& root, const bool allow_package_scope) {
        auto commands = read_task_declarations(root, allow_package_scope);
        if (!commands)
            return std::unexpected(commands.error());

        auto workflows = read_workflow_declarations(root, allow_package_scope);
        if (!workflows)
            return std::unexpected(workflows.error());

        return AutomationDocument{std::move(*commands), std::move(*workflows)};
    }

    namespace {
        Result<std::optional<Manifest>> read_document_package(TableReader& root) {
            auto package_result = root.optional_table("package");
            if (!package_result)
                return std::unexpected(package_result.error());

            if (!*package_result)
                return std::optional<Manifest>{};

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

            auto resolver = package.optional_string("resolver");
            if (!resolver)
                return std::unexpected(resolver.error());

            if (*resolver) {
                if (!is_valid_identifier(**resolver)) {
                    return std::unexpected(error_at(package.location_of("resolver"), "`" + **resolver + "` is not a valid resolver name"));
                }
                manifest.resolver = std::move(**resolver);
            }

            auto tests = read_target_references(package, "tests", PackageTargetKind::test, manifest.target_references);
            if (!tests)
                return std::unexpected(tests.error());

            auto examples = read_target_references(package, "examples", PackageTargetKind::example, manifest.target_references);
            if (!examples)
                return std::unexpected(examples.error());

            auto benchmarks = read_target_references(package, "benchmarks", PackageTargetKind::benchmark, manifest.target_references);
            if (!benchmarks)
                return std::unexpected(benchmarks.error());

            auto package_finished = package.finish();
            if (!package_finished)
                return std::unexpected(package_finished.error());

            auto dependencies = read_dependencies(root);
            if (!dependencies)
                return std::unexpected(dependencies.error());

            manifest.dependencies = std::move(*dependencies);

            auto public_dependencies = read_dependencies(root, "public-dependencies", DependencyVisibility::public_dependency);
            if (!public_dependencies)
                return std::unexpected(public_dependencies.error());

            manifest.dependencies.insert(
                manifest.dependencies.end(),
                std::make_move_iterator(public_dependencies->begin()),
                std::make_move_iterator(public_dependencies->end())
            );

            auto features = read_features(root, manifest.default_features);
            if (!features)
                return std::unexpected(features.error());

            manifest.features = std::move(*features);

            auto products = read_products(root, manifest);
            if (!products)
                return std::unexpected(products.error());

            auto targets = parse_package_targets(root, manifest.resolver, manifest.targets);
            if (!targets)
                return std::unexpected(targets.error());

            if (!manifest.resolver.empty()) {
                if (const Value* options = root.take(manifest.resolver)) {
                    if (!options->is_table())
                        return std::unexpected(error_at(options->location(), "resolver options must be a table"));

                    manifest.resolver_options = *options;
                }
            }
            if (manifest.resolver.empty() && !manifest.products.empty()) {
                return std::unexpected(
                    error_at(manifest.location, "package `" + manifest.name + "` declares a product without a resolver")
                );
            }

            return std::optional<Manifest>{std::move(manifest)};
        }

        Result<std::optional<PackageSet>> read_document_package_set(TableReader& root) {
            auto package_set_result = root.optional_table("package-set");
            if (!package_set_result)
                return std::unexpected(package_set_result.error());

            if (!*package_set_result)
                return std::optional<PackageSet>{};

            TableReader package_set = std::move(**package_set_result);
            auto parsed = parse_package_set(package_set);
            if (!parsed)
                return std::unexpected(parsed.error());

            return std::optional<PackageSet>{std::move(*parsed)};
        }

        Result<void> read_document_members(TableReader& root, ManifestDocument& result) {
            auto members_result = root.optional_table("members");
            if (!members_result)
                return std::unexpected(members_result.error());

            if (!*members_result)
                return {};

            TableReader members = std::move(**members_result);
            for (const TableEntry& entry: members.entries()) {
                auto member = parse_inline_member(entry, join_config_path(members.path(), entry.key));
                if (!member)
                    return std::unexpected(member.error());

                result.inline_members.push_back(std::move(*member));
            }
            members.take_all();
            return {};
        }

        Result<void> read_document_routing(TableReader& root, ManifestDocument& result) {
            auto routing_result = root.optional_table("routing");
            if (!routing_result)
                return std::unexpected(routing_result.error());

            if (!*routing_result)
                return {};

            TableReader routing = std::move(**routing_result);
            for (const TableEntry& entry: routing.entries()) {
                const std::string* provider = entry.value.as_string();
                if (!provider)
                    return std::unexpected(error_at(entry.value.location(), "provider routing values must be strings"));

                if (!is_valid_identifier(*provider)) {
                    return std::unexpected(error_at(entry.value.location(), "`" + *provider + "` is not a valid provider name"));
                }
                result.routing.emplace(entry.key, *provider);
            }
            routing.take_all();
            return {};
        }

        Result<void> read_document_automation(TableReader& root, ManifestDocument& result) {
            auto commands = read_task_declarations(root);
            if (!commands)
                return std::unexpected(commands.error());

            if (!commands->empty()) {
                if (!result.package)
                    return std::unexpected(error_at(commands->front().location, "commands require a package"));

                result.package->commands = std::move(*commands);
            }

            auto workflows = read_workflow_declarations(root);
            if (!workflows)
                return std::unexpected(workflows.error());

            if (!workflows->empty()) {
                if (!result.package)
                    return std::unexpected(error_at(workflows->front().location, "workflows require a package"));

                result.package->workflows = std::move(*workflows);
            }

            return {};
        }
    }

    Result<ManifestDocument> parse_manifest_document(const Value& document) {
        auto root_result = TableReader::bind(document);
        if (!root_result)
            return std::unexpected(root_result.error());

        TableReader root = std::move(*root_result);
        ManifestDocument result;

        auto package = read_document_package(root);
        if (!package)
            return std::unexpected(package.error());

        result.package = std::move(*package);

        auto package_set = read_document_package_set(root);
        if (!package_set)
            return std::unexpected(package_set.error());

        result.package_set = std::move(*package_set);

        auto members = read_document_members(root, result);
        if (!members)
            return std::unexpected(members.error());

        if (!result.package && !result.package_set) {
            return std::unexpected(error_at(document.location(), "manifest requires a `[package]` or `[package-set]` table"));
        }
        if (result.package && result.package_set && !result.package_set->defaults.empty()) {
            return std::unexpected(
                error_at(result.package_set->defaults_location, "`[package]` is already the root; `package-set.default` cannot be declared")
                    .add_note("remove `default` or replace `[package]` with package set members")
            );
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

        auto routing = read_document_routing(root, result);
        if (!routing)
            return std::unexpected(routing.error());

        auto imports = root.string_array("imports");
        if (!imports)
            return std::unexpected(imports.error());

        for (std::string& import: *imports)
            result.imports.emplace_back(std::move(import));

        auto automation = read_document_automation(root, result);
        if (!automation)
            return std::unexpected(automation.error());

        auto root_finished = root.finish();
        if (!root_finished)
            return std::unexpected(root_finished.error());

        return result;
    }

    namespace {
        Result<ManifestDocument> complete_manifest_document(const Value& document, const std::filesystem::path& path) {
            auto manifest = parse_manifest_document(document);
            if (!manifest)
                return std::unexpected(manifest.error());

            manifest->source = path;
            if (manifest->package) {
                manifest->package->source = path;
                for (PackageTarget& target: manifest->package->targets)
                    target.source = path;
            }
            for (Manifest& member: manifest->inline_members)
                member.source = path;

            std::vector<std::filesystem::path> imports = manifest->imports;
            if (manifest->package_set && manifest->package_set->provider_config)
                imports.push_back(*manifest->package_set->provider_config);

            std::set<std::filesystem::path> loading;
            for (const std::filesystem::path& import: imports) {
                auto appended = append_document_fragment(*manifest, path.parent_path() / import, loading);
                if (!appended)
                    return std::unexpected(appended.error());
            }
            return manifest;
        }

        Result<std::vector<PackageTarget>> parse_package_targets_document(
            const Value& document,
            const std::filesystem::path& path,
            const PackageTargetKind kind,
            const std::string_view resolver
        ) {
            auto root_result = TableReader::bind(document);
            if (!root_result)
                return std::unexpected(root_result.error());

            TableReader root = std::move(*root_result);

            std::vector<PackageTarget> targets;
            auto parsed = parse_package_targets(root, resolver, targets, kind, true);
            if (!parsed)
                return std::unexpected(parsed.error());

            if (targets.empty()) {
                return std::unexpected(error_at(document.location(), "referenced target manifest declares no targets"));
            }

            auto commands = read_task_declarations(root);
            if (!commands)
                return std::unexpected(commands.error());

            auto finished = root.finish();
            if (!finished)
                return std::unexpected(finished.error());

            for (PackageTarget& target: targets) {
                target.source = path;
                target.commands = *commands;
            }

            return targets;
        }
    }

    Result<ManifestDocument> parse_manifest_document_file(const std::filesystem::path& path) {
        auto document = parse_file(path);
        if (!document)
            return std::unexpected(document.error());

        auto manifest = complete_manifest_document(*document, path);
        if (!manifest)
            return std::unexpected(manifest.error());
        return manifest;
    }

    std::string_view target_manifest_filename(const PackageTargetKind kind) noexcept {
        switch (kind) {
        case PackageTargetKind::test: return "Kaixa.test.toml";
        case PackageTargetKind::example: return "Kaixa.example.toml";
        case PackageTargetKind::benchmark: return "Kaixa.benchmark.toml";
        }
        return {};
    }

    Result<KaixaDocument> parse_kaixa_document_file(const std::filesystem::path& path) {
        auto document = parse_file(path);
        if (!document)
            return std::unexpected(document.error());

        if (path.filename() == "Kaixa.toml") {
            auto manifest = complete_manifest_document(*document, path);
            if (!manifest)
                return std::unexpected(manifest.error());

            return KaixaDocument{std::move(*manifest)};
        }

        std::optional<PackageTargetKind> kind;
        for (const PackageTargetKind candidate: {PackageTargetKind::test, PackageTargetKind::example, PackageTargetKind::benchmark}) {
            if (path.filename() == target_manifest_filename(candidate)) {
                kind = candidate;
                break;
            }
        }
        if (!kind)
            return std::unexpected(error("unrecognized Kaixa document filename: " + path.filename().string()));

        if (document->find("package") || document->find("package-set")) {
            return std::unexpected(error_at(document->location(), "associated-target documents cannot declare packages or package sets"));
        }

        auto targets = parse_package_targets_document(*document, path, *kind, {});
        if (!targets)
            return std::unexpected(targets.error());

        return KaixaDocument{TargetManifestDocument{path, std::move(*targets)}};
    }

    Result<ManifestDocument> parse_manifest_document_string(const std::string_view text, const std::string_view source_name) {
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
            return std::unexpected(error_at({}, "manifest `" + std::string(source_name) + "` does not declare a package"));
        }

        return std::move(*document->package);
    }

    Result<std::vector<PackageTarget>> parse_package_targets_file(
        const std::filesystem::path& path,
        const PackageTargetKind kind,
        const std::string_view resolver
    ) {
        auto document = parse_file(path);
        if (!document)
            return std::unexpected(document.error());

        return parse_package_targets_document(*document, path, kind, resolver);
    }

    Result<std::vector<PackageTarget>> parse_package_targets(
        const Value& document,
        const std::filesystem::path& path,
        const PackageTargetKind kind,
        const std::string_view resolver
    ) {
        return parse_package_targets_document(document, path, kind, resolver);
    }

    Result<ManifestTree> load_manifest_tree(const std::filesystem::path& root) {
        std::error_code failure;
        if (!std::filesystem::is_directory(root, failure)) {
            return std::unexpected(error(
                failure ? "cannot inspect manifest tree `" + root.string() + "`: " + failure.message()
                        : "manifest tree is not a directory: " + root.string()
            ));
        }

        std::vector<std::filesystem::path> manifests;
        std::filesystem::recursive_directory_iterator iterator(root, std::filesystem::directory_options::skip_permission_denied, failure);
        const std::filesystem::recursive_directory_iterator end;
        while (!failure && iterator != end) {
            if (iterator->is_directory(failure) && iterator->path().filename() == ".kaixa") {
                iterator.disable_recursion_pending();
            } else if (iterator->is_regular_file(failure)) {
                const std::filesystem::path filename = iterator->path().filename();
                const bool package_document = filename == "Kaixa.toml";
                constexpr std::array target_kinds{PackageTargetKind::test, PackageTargetKind::example, PackageTargetKind::benchmark};
                const bool target_document = std::ranges::any_of(target_kinds, [&](const PackageTargetKind kind) {
                    return filename == target_manifest_filename(kind);
                });
                if (package_document || target_document)
                    manifests.push_back(iterator->path());
            }
            iterator.increment(failure);
        }
        if (failure) {
            return std::unexpected(error("cannot enumerate manifest tree `" + root.string() + "`: " + failure.message()));
        }
        std::ranges::sort(manifests);

        ManifestTree tree;
        for (const std::filesystem::path& path: manifests) {
            auto document = parse_kaixa_document_file(path);
            if (!document)
                return std::unexpected(document.error());

            if (const auto* package = std::get_if<ManifestDocument>(&*document)) {
                tree.summary.packages += package->package ? 1 : 0;
                tree.summary.packages += package->inline_members.size();
                tree.summary.package_sets += package->package_set ? 1 : 0;
            } else {
                ++tree.summary.target_documents;
            }
            tree.documents.push_back(std::move(*document));
            ++tree.summary.documents;
        }
        return tree;
    }

    Result<ManifestTreeSummary> validate_manifest_tree(const std::filesystem::path& root) {
        auto tree = load_manifest_tree(root);
        if (!tree)
            return std::unexpected(tree.error());

        return tree->summary;
    }
}
