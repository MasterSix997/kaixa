#include <kaixa/model/manifest.hpp>

#include <kaixa/config/value_operations.hpp>
#include <kaixa/foundation/filesystem.hpp>

#include <algorithm>
#include <string_view>

namespace kaixa {
    namespace {
        std::string key(const std::string_view name) {
            return toml_key(name);
        }

        std::string_view target_section(const PackageTarget& target) {
            switch (target.kind) {
            case PackageTargetKind::test: return target.each_source ? "tests" : "test";
            case PackageTargetKind::example: return target.each_source ? "examples" : "example";
            case PackageTargetKind::benchmark: return target.each_source ? "benchmarks" : "benchmark";
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

        std::vector<std::string> path_strings(const std::vector<std::filesystem::path>& paths) {
            std::vector<std::string> result;
            result.reserve(paths.size());
            for (const std::filesystem::path& path: paths)
                result.push_back(path.generic_string());

            return result;
        }

        Result<std::string> format_value(const Value& value) {
            return format_inline_toml(value);
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
                    return std::unexpected(error("duplicate manifest value key `" + entry.key + "`"));
                }
                auto formatted = format_value(entry.value);
                if (!formatted)
                    return std::unexpected(formatted.error());

                output += key(entry.key) + " = " + *formatted + '\n';
            }
            return {};
        }

        Result<void> append_dotted_table(std::string& output, const std::string& prefix, const Value& value) {
            const std::vector<TableEntry>* entries = value.as_table();
            if (!entries)
                return std::unexpected(error("manifest resolver settings must be a table"));

            if (entries->empty()) {
                output += prefix + " = {}\n";
                return {};
            }

            for (std::size_t index = 0; index < entries->size(); ++index) {
                const TableEntry& entry = (*entries)[index];
                const auto duplicate = std::ranges::find_if(
                    entries->begin(),
                    entries->begin() + static_cast<std::ptrdiff_t>(index),
                    [&](const TableEntry& candidate) { return candidate.key == entry.key; }
                );
                if (duplicate != entries->begin() + static_cast<std::ptrdiff_t>(index))
                    return std::unexpected(error("duplicate manifest value key `" + entry.key + "`"));

                const std::string path = prefix + '.' + key(entry.key);
                if (const std::vector<TableEntry>* table = entry.value.as_table()) {
                    if (table->empty()) {
                        output += path + " = {}\n";
                    } else {
                        auto appended = append_dotted_table(output, path, entry.value);
                        if (!appended)
                            return std::unexpected(appended.error());
                    }
                    continue;
                }

                auto formatted = format_value(entry.value);
                if (!formatted)
                    return std::unexpected(formatted.error());

                output += path + " = " + *formatted + '\n';
            }
            return {};
        }

        void append_header(std::string& output, std::initializer_list<std::string_view> path);
        void append_array_header(std::string& output, std::initializer_list<std::string_view> path);

        Result<void> append_dependency(std::string& output, const DependencyBinding& dependency) {
            output += key(dependency.request.package) + " = ";
            const bool shorthand = dependency.request.version
                && dependency.request.features.empty()
                && !dependency.request.optional
                && !dependency.alias
                && dependency.selection.automatic();
            if (shorthand) {
                output += toml_string(dependency.request.version->text) + '\n';
                return {};
            }

            output += "{ ";
            bool first = true;
            const auto append_field = [&](const std::string_view name, const std::string& value) {
                if (!first)
                    output += ", ";

                output += key(name) + " = " + value;
                first = false;
            };

            if (dependency.request.version)
                append_field("version", toml_string(dependency.request.version->text));

            if (!dependency.request.features.empty()) {
                std::string features = "[";
                for (std::size_t index = 0; index < dependency.request.features.size(); ++index) {
                    if (index != 0)
                        features += ", ";

                    features += toml_string(dependency.request.features[index]);
                }
                features += ']';
                append_field("features", features);
            }
            if (dependency.request.optional)
                append_field("optional", "true");

            if (dependency.alias)
                append_field("alias", toml_string(*dependency.alias));

            if (const std::string* provider = dependency.selection.provider())
                append_field("from", toml_string(*provider));

            if (const std::filesystem::path* path = dependency.selection.path())
                append_field("path", toml_string(path->generic_string()));

            if (const SourceLocator* source = dependency.selection.source()) {
                auto options = format_value(source->options);
                if (!options)
                    return std::unexpected(options.error());

                append_field(source->driver, *options);
            }
            output += " }\n";
            return {};
        }

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
                output += std::string(target.name_template ? "name-template = " : "name = ") + toml_string(*target.name) + '\n';

            if (target.display_name)
                output += "display-name = " + toml_string(*target.display_name) + '\n';

            if (target.description)
                output += "description = " + toml_string(*target.description) + '\n';

            if (target.category)
                output += "category = " + toml_string(*target.category) + '\n';

            append_strings(output, "sources", target.sources.include);
            if (!target.sources.exclude.empty())
                append_strings(output, "source-excludes", target.sources.exclude);

            if (!target.include_directories.empty())
                append_strings(output, "include", target.include_directories);

            if (!target.system_include_directories.empty())
                append_strings(output, "system-include", target.system_include_directories);

            if (!target.definitions.empty()) {
                auto definitions = format_value(Value::table(target.definitions, target.location));
                if (!definitions)
                    return std::unexpected(definitions.error());

                output += "defines = " + *definitions + '\n';
            }

            if (!target.system_libraries.empty())
                append_strings(output, "system-libraries", target.system_libraries);

            if (!target.required_features.empty())
                append_strings(output, "required-features", target.required_features);

            if (!target.arguments.empty())
                append_strings(output, "arguments", target.arguments);

            if (target.discover)
                output += "discover = true\n";

            if (target.hidden)
                output += "hidden = true\n";

            if (target.install)
                output += "install = true\n";

            if (target.framework)
                output += "framework = " + toml_string(*target.framework) + '\n';

            if (target.matrix) {
                std::vector<TableEntry> axes;
                axes.reserve(target.matrix->axes.size());
                for (const TargetMatrixAxis& axis: target.matrix->axes)
                    axes.push_back({axis.name, Value::array(axis.values, axis.location)});

                auto formatted = format_value(Value::table(std::move(axes), target.matrix->location));
                if (!formatted)
                    return std::unexpected(formatted.error());

                output += "matrix = " + *formatted + '\n';
            }

            if (!target.dependencies.empty()) {
                append_header(output, {section, "dependencies"});
                for (const DependencyBinding& dependency: target.dependencies) {
                    auto appended = append_dependency(output, dependency);
                    if (!appended)
                        return std::unexpected(appended.error());
                }
            }

            if (target.resolver_options) {
                append_header(output, {section, resolver});
                auto appended = append_table(output, *target.resolver_options);
                if (!appended)
                    return std::unexpected(appended.error());
            }

            return {};
        }

        Result<void> append_resolver_document(std::string& output, const std::string_view resolver, const Value& value) {
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
                    return std::unexpected(error("duplicate manifest value key `" + entry.key + "`"));
                }

                const std::vector<Value>* array = entry.value.as_array();
                const bool tables = array
                    && !array->empty()
                    && std::ranges::all_of(*array, [](const Value& item) { return item.is_table(); });
                if (tables)
                    continue;

                auto formatted = format_value(entry.value);
                if (!formatted)
                    return std::unexpected(formatted.error());

                output += key(entry.key) + " = " + *formatted + '\n';
            }

            for (const TableEntry& entry: *entries) {
                const std::vector<Value>* array = entry.value.as_array();
                if (!array || array->empty() || !std::ranges::all_of(*array, [](const Value& item) { return item.is_table(); })) {
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

        void append_task(std::string& output, const TaskDeclaration& task) {
            append_header(output, {"command", task.name});
            append_strings(output, "run", task.run);
            if (task.working_directory)
                output += "working-directory = " + toml_string(task.working_directory->generic_string()) + '\n';

            if (!task.inputs.empty())
                append_strings(output, "inputs", path_strings(task.inputs));

            if (!task.outputs.empty())
                append_strings(output, "outputs", path_strings(task.outputs));

            if (!task.after.empty())
                append_strings(output, "after", task.after);

            if (!task.environment.empty()) {
                output += "environment = { ";
                bool first = true;
                for (const auto& [name, value]: task.environment) {
                    if (!first)
                        output += ", ";

                    output += key(name) + " = " + toml_string(value);
                    first = false;
                }
                output += " }\n";
            }
        }

        void append_workflow(std::string& output, const WorkflowDeclaration& workflow) {
            append_header(output, {"workflow", workflow.name});
            append_strings(output, "steps", workflow.steps);
        }

        Result<void> validate_tasks(const std::vector<TaskDeclaration>& tasks) {
            for (std::size_t index = 0; index < tasks.size(); ++index) {
                const TaskDeclaration& task = tasks[index];
                if (!is_valid_target_name(task.name))
                    return std::unexpected(error("invalid command name `" + task.name + "`"));

                if (task.package)
                    return std::unexpected(error("manifest command `" + task.name + "` cannot select a package"));

                if (task.run.empty())
                    return std::unexpected(error("command `" + task.name + "` has an empty run vector"));

                if (std::ranges::any_of(task.run, &std::string::empty))
                    return std::unexpected(error("command `" + task.name + "` has an empty argument"));

                const auto duplicate = std::ranges::find(
                    tasks.begin(),
                    tasks.begin() + static_cast<std::ptrdiff_t>(index),
                    task.name,
                    &TaskDeclaration::name
                );
                if (duplicate != tasks.begin() + static_cast<std::ptrdiff_t>(index))
                    return std::unexpected(error("duplicate command `" + task.name + "`"));
            }
            return {};
        }

        Result<void> validate_workflows(const std::vector<WorkflowDeclaration>& workflows) {
            for (std::size_t index = 0; index < workflows.size(); ++index) {
                const WorkflowDeclaration& workflow = workflows[index];
                if (!is_valid_target_name(workflow.name))
                    return std::unexpected(error("invalid workflow name `" + workflow.name + "`"));

                if (workflow.package)
                    return std::unexpected(error("manifest workflow `" + workflow.name + "` cannot select a package"));

                if (workflow.steps.empty())
                    return std::unexpected(error("workflow `" + workflow.name + "` has no steps"));

                if (std::ranges::any_of(workflow.steps, &std::string::empty))
                    return std::unexpected(error("workflow `" + workflow.name + "` has an empty step"));

                const auto duplicate = std::ranges::find(
                    workflows.begin(),
                    workflows.begin() + static_cast<std::ptrdiff_t>(index),
                    workflow.name,
                    &WorkflowDeclaration::name
                );
                if (duplicate != workflows.begin() + static_cast<std::ptrdiff_t>(index))
                    return std::unexpected(error("duplicate workflow `" + workflow.name + "`"));
            }
            return {};
        }

        Result<void> validate_dependency(const DependencyBinding& dependency) {
            if (!is_valid_package_name(dependency.request.package))
                return std::unexpected(error("invalid dependency name `" + dependency.request.package + "`"));

            if (dependency.request.version) {
                auto requirement = parse_version_requirement(dependency.request.version->text);
                if (!requirement)
                    return std::unexpected(requirement.error());
            }
            if (dependency.alias && !is_valid_identifier(*dependency.alias))
                return std::unexpected(error("invalid dependency alias `" + *dependency.alias + "`"));

            if (const std::filesystem::path* path = dependency.selection.path(); path && path->empty())
                return std::unexpected(error("dependency `" + dependency.request.package + "` has an empty path"));

            const SourceLocator* source = dependency.selection.source();
            if (source && (!is_valid_identifier(source->driver) || !source->options.is_table())) {
                return std::unexpected(error("dependency `" + dependency.request.package + "` has an invalid source"));
            }

            return {};
        }

        Result<void> validate_dependencies(const std::vector<DependencyBinding>& dependencies) {
            for (std::size_t index = 0; index < dependencies.size(); ++index) {
                const DependencyBinding& dependency = dependencies[index];
                auto valid = validate_dependency(dependency);
                if (!valid)
                    return std::unexpected(valid.error());

                const auto previous_end = dependencies.begin() + static_cast<std::ptrdiff_t>(index);
                const auto duplicate = std::ranges::find_if(dependencies.begin(), previous_end, [&](const DependencyBinding& candidate) {
                    return candidate.request.package == dependency.request.package;
                });
                if (duplicate != previous_end)
                    return std::unexpected(error("duplicate dependency `" + dependency.request.package + "`"));
            }
            return {};
        }

        Result<void> validate_target(const PackageTarget& target) {
            if (target.name && !is_valid_target_name(*target.name))
                return std::unexpected(error("invalid package target name `" + *target.name + "`"));

            if (target.sources.include.empty())
                return std::unexpected(error("package target requires source patterns"));

            for (const DependencyBinding& dependency: target.dependencies) {
                auto valid = validate_dependency(dependency);
                if (!valid)
                    return std::unexpected(valid.error());
            }

            if (target.resolver_options && !target.resolver_options->is_table())
                return std::unexpected(error("package target resolver options must be a table"));

            return {};
        }

        Result<void> validate_targets(const Manifest& manifest) {
            for (const PackageTargetReference& reference: manifest.target_references) {
                if (reference.path.empty())
                    return std::unexpected(error("package target manifest path cannot be empty"));
            }
            for (const PackageTarget& target: manifest.targets) {
                auto valid = validate_target(target);
                if (!valid)
                    return std::unexpected(valid.error());
            }
            return {};
        }

        Result<void> validate_configuration(const ConfigurationDefinition& configuration) {
            for (std::size_t index = 0; index < configuration.resolvers.size(); ++index) {
                const ResolverConfigurationDefinition& resolver = configuration.resolvers[index];
                if (!is_valid_identifier(resolver.resolver)) {
                    return std::unexpected(
                        error("invalid resolver name `" + resolver.resolver + "` in build configuration `" + configuration.name + "`")
                    );
                }
                if (!resolver.settings.is_table())
                    return std::unexpected(error("settings for resolver `" + resolver.resolver + "` must be a table"));

                const auto previous_end = configuration.resolvers.begin() + static_cast<std::ptrdiff_t>(index);
                const auto duplicate = std::ranges::find_if(
                    configuration.resolvers.begin(),
                    previous_end,
                    [&](const ResolverConfigurationDefinition& candidate) { return candidate.resolver == resolver.resolver; }
                );
                if (duplicate != previous_end) {
                    return std::unexpected(
                        error("duplicate resolver `" + resolver.resolver + "` in build configuration `" + configuration.name + "`")
                    );
                }
            }
            return {};
        }

        Result<void> validate_configurations(const ConfigurationSet& configurations) {
            for (std::size_t index = 0; index < configurations.definitions.size(); ++index) {
                const ConfigurationDefinition& configuration = configurations.definitions[index];
                if (configuration.name.empty())
                    return std::unexpected(error("build configuration name cannot be empty"));

                const auto previous_end = configurations.definitions.begin() + static_cast<std::ptrdiff_t>(index);
                const auto duplicate = std::ranges::find_if(
                    configurations.definitions.begin(),
                    previous_end,
                    [&](const ConfigurationDefinition& candidate) { return candidate.name == configuration.name; }
                );
                if (duplicate != previous_end)
                    return std::unexpected(error("duplicate build configuration `" + configuration.name + "`"));

                auto valid = validate_configuration(configuration);
                if (!valid)
                    return std::unexpected(valid.error());
            }
            return {};
        }

        Result<void> validate_manifest(const Manifest& manifest) {
            if (!is_valid_package_name(manifest.name))
                return std::unexpected(error("invalid package name `" + manifest.name + "`"));

            if (!is_valid_identifier(manifest.resolver))
                return std::unexpected(error("invalid resolver name `" + manifest.resolver + "`"));

            if (manifest.version && manifest.version->text.empty())
                return std::unexpected(error("package version cannot be empty"));

            if (manifest.version) {
                auto version = parse_version(manifest.version->text);
                if (!version)
                    return std::unexpected(version.error());
            }

            auto dependencies = validate_dependencies(manifest.dependencies);
            if (!dependencies)
                return std::unexpected(dependencies.error());

            if (manifest.resolver_options && !manifest.resolver_options->is_table())
                return std::unexpected(error("manifest resolver options must be a table"));

            auto tasks = validate_tasks(manifest.commands);
            if (!tasks)
                return std::unexpected(tasks.error());

            auto workflows = validate_workflows(manifest.workflows);
            if (!workflows)
                return std::unexpected(workflows.error());

            auto targets = validate_targets(manifest);
            if (!targets)
                return std::unexpected(targets.error());

            return validate_configurations(manifest.configurations);
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

        for (const PackageTargetKind kind: {PackageTargetKind::test, PackageTargetKind::example, PackageTargetKind::benchmark}) {
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
            for (const DependencyBinding& dependency: manifest.dependencies) {
                auto appended = append_dependency(output, dependency);
                if (!appended)
                    return std::unexpected(appended.error());
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
                    auto appended = append_dotted_table(output, key(resolver.resolver), resolver.settings);
                    if (!appended)
                        return std::unexpected(appended.error());
                }
            }
        }

        if (manifest.resolver_options) {
            auto appended = append_resolver_document(output, manifest.resolver, *manifest.resolver_options);
            if (!appended)
                return std::unexpected(appended.error());
        }

        for (const PackageTarget& target: manifest.targets) {
            const std::string_view section = target_section(target);
            const std::size_t count = static_cast<std::size_t>(std::ranges::count_if(manifest.targets, [&](const PackageTarget& candidate) {
                return target_section(candidate) == section;
            }));
            auto appended = append_package_target(output, target, manifest.resolver, count > 1);
            if (!appended)
                return std::unexpected(appended.error());
        }

        for (const TaskDeclaration& task: manifest.commands)
            append_task(output, task);

        for (const WorkflowDeclaration& workflow: manifest.workflows)
            append_workflow(output, workflow);

        return output;
    }

    Result<void> write_manifest_file(const std::filesystem::path& path, const Manifest& manifest) {
        auto contents = format_manifest(manifest);
        if (!contents)
            return std::unexpected(contents.error());

        return write_file(path, *contents);
    }
}
