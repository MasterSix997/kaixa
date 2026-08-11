#include "configuration.hpp"

#include <kaixa/config/table_reader.hpp>

#include <algorithm>
#include <string_view>
#include <utility>

namespace kaixa::plugin::cmake::detail {
    namespace {
        Diagnostic wrong_kind(
            SourceLocation location,
            const std::string_view expected,
            const ValueKind found
        ) {
            return error_at(
                std::move(location),
                "expected " + std::string(expected) + ", found " + std::string(value_kind_name(found))
            );
        }

        Result<std::vector<std::string>> string_array(TableReader& table, const std::string_view key) {
            const Value* value = table.take(key);
            if (!value)
                return std::vector<std::string>{};

            const std::vector<Value>* array = value->as_array();
            if (!array)
                return std::unexpected(wrong_kind(table.location_of(key), "an array", value->kind()));

            std::vector<std::string> result;
            result.reserve(array->size());
            for (const Value& item: *array) {
                const std::string* text = item.as_string();
                if (!text) {
                    SourceLocation location = item.location();
                    location.config_path = table.location_of(key).config_path;
                    return std::unexpected(wrong_kind(
                        std::move(location),
                        "a string array element",
                        item.kind()
                    ));
                }
                if (text->empty())
                    return std::unexpected(error_at(item.location(), "array values cannot be empty"));
                result.push_back(*text);
            }
            return result;
        }

        Result<std::optional<std::int64_t>> optional_integer(TableReader& table, const std::string_view key) {
            const Value* value = table.take(key);
            if (!value)
                return std::nullopt;
            const std::int64_t* integer = value->as_integer();
            if (!integer)
                return std::unexpected(wrong_kind(table.location_of(key), "an integer", value->kind()));
            return *integer;
        }

        Result<TargetOptions> read_target(std::string name, TableReader target) {
            TargetOptions result;
            result.name = std::move(name);

            auto type = target.string("type");
            if (!type)
                return std::unexpected(type.error());
            if (*type == "executable") {
                result.type = TargetType::executable;
            } else if (*type == "static-library") {
                result.type = TargetType::static_library;
            } else if (*type == "shared-library") {
                result.type = TargetType::shared_library;
            } else if (*type == "interface-library") {
                result.type = TargetType::interface_library;
            } else {
                return std::unexpected(error_at(
                    target.location_of("type"),
                    "unknown target type `" + *type + "`"
                ).add_note(
                    "expected `executable`, `static-library`, `shared-library` or `interface-library`"
                ));
            }

            auto sources = string_array(target, "sources");
            if (!sources)
                return std::unexpected(sources.error());
            result.sources = std::move(*sources);

            auto includes = string_array(target, "include-directories");
            if (!includes)
                return std::unexpected(includes.error());
            result.include_directories = std::move(*includes);

            auto public_includes = string_array(target, "public-include-directories");
            if (!public_includes)
                return std::unexpected(public_includes.error());
            result.public_include_directories = std::move(*public_includes);

            auto system_includes = string_array(target, "system-include-directories");
            if (!system_includes)
                return std::unexpected(system_includes.error());
            result.system_include_directories = std::move(*system_includes);

            auto public_system_includes = string_array(
                target,
                "public-system-include-directories"
            );
            if (!public_system_includes)
                return std::unexpected(public_system_includes.error());
            result.public_system_include_directories = std::move(*public_system_includes);

            auto links = string_array(target, "link-libraries");
            if (!links)
                return std::unexpected(links.error());
            result.link_libraries = std::move(*links);

            auto public_links = string_array(target, "public-link-libraries");
            if (!public_links)
                return std::unexpected(public_links.error());
            result.public_link_libraries = std::move(*public_links);

            auto definitions = string_array(target, "compile-definitions");
            if (!definitions)
                return std::unexpected(definitions.error());
            result.compile_definitions = std::move(*definitions);

            auto public_definitions = string_array(target, "public-compile-definitions");
            if (!public_definitions)
                return std::unexpected(public_definitions.error());
            result.public_compile_definitions = std::move(*public_definitions);

            auto compile_options = string_array(target, "compile-options");
            if (!compile_options)
                return std::unexpected(compile_options.error());
            result.compile_options = std::move(*compile_options);

            auto public_compile_options = string_array(target, "public-compile-options");
            if (!public_compile_options)
                return std::unexpected(public_compile_options.error());
            result.public_compile_options = std::move(*public_compile_options);

            auto standard = optional_integer(target, "cxx-standard");
            if (!standard)
                return std::unexpected(standard.error());
            result.cxx_standard = *standard;

            if (result.type != TargetType::interface_library && result.sources.empty()) {
                return std::unexpected(error_at(
                    target.location_of("sources"),
                    "a compiled target requires at least one source"
                ));
            }
            if (result.type == TargetType::interface_library
                && (!result.sources.empty() || !result.include_directories.empty()
                    || !result.system_include_directories.empty()
                    || !result.link_libraries.empty() || !result.compile_definitions.empty()
                    || !result.compile_options.empty())) {
                return std::unexpected(error_at(
                    target.location_of("type"),
                    "an interface library cannot have private target properties"
                ));
            }
            if (result.cxx_standard && *result.cxx_standard <= 0) {
                return std::unexpected(error_at(
                    target.location_of("cxx-standard"),
                    "C++ standard must be positive"
                ));
            }

            auto finished = target.finish();
            if (!finished)
                return std::unexpected(finished.error());
            return result;
        }

        Result<TestOptions> read_test(std::string name, TableReader test) {
            TestOptions result;
            result.name = std::move(name);

            auto target = test.string("target");
            if (!target)
                return std::unexpected(target.error());
            result.target = std::move(*target);

            auto arguments = string_array(test, "arguments");
            if (!arguments)
                return std::unexpected(arguments.error());
            result.arguments = std::move(*arguments);

            auto finished = test.finish();
            if (!finished)
                return std::unexpected(finished.error());
            return result;
        }

        std::string quote(const std::string_view value) {
            std::string equals;
            while (value.contains("]" + equals + "]"))
                equals += '=';
            return "[" + equals + "[" + std::string(value) + "]" + equals + "]";
        }

        void emit_values(
            std::string& output,
            const std::string_view command,
            const std::string& target,
            const std::string_view scope,
            const std::vector<std::string>& values
        ) {
            if (values.empty())
                return;
            output += std::string(command) + "(" + target + " " + std::string(scope) + "\n";
            for (const std::string& value: values)
                output += "    " + quote(value) + "\n";
            output += ")\n\n";
        }

        std::vector<std::string> project_paths(
            const Options& options,
            const std::vector<std::string>& values
        ) {
            if (options.generation == GenerationMode::source)
                return values;

            std::vector<std::string> result;
            result.reserve(values.size());
            for (const std::string& value: values) {
                const std::filesystem::path path = value;
                if (path.is_absolute() || value.starts_with("$<"))
                    result.push_back(value);
                else
                    result.push_back((options.source / path).lexically_normal().generic_string());
            }
            return result;
        }
    }

    Result<Options> read_options(const Graph& graph, const PackageNode& package) {
        Options result;
        result.source = package.directory;
        result.languages = {"CXX"};
        if (!package.manifest || !package.manifest->resolver_options)
            return result;

        auto options_result = TableReader::bind(*package.manifest->resolver_options, "cmake");
        if (!options_result)
            return std::unexpected(options_result.error());
        TableReader options = std::move(*options_result);

        auto source = options.optional_string("source");
        if (!source)
            return std::unexpected(source.error());
        if (*source)
            result.source /= **source;

        auto generation = options.optional_string("generation");
        if (!generation)
            return std::unexpected(generation.error());
        if (*generation) {
            if (**generation == "source") {
                result.generation = GenerationMode::source;
            } else if (**generation == "state") {
                result.generation = GenerationMode::state;
            } else {
                return std::unexpected(error_at(
                    options.location_of("generation"),
                    "unknown CMake generation location `" + **generation
                        + "`; expected `source` or `state`"
                ));
            }
        }

        const Value* languages_value = options.take("languages");
        if (languages_value) {
            const std::vector<Value>* languages = languages_value->as_array();
            if (!languages) {
                return std::unexpected(wrong_kind(
                    options.location_of("languages"),
                    "an array",
                    languages_value->kind()
                ));
            }
            result.languages.clear();
            for (const Value& item: *languages) {
                const std::string* language = item.as_string();
                if (!language)
                    return std::unexpected(wrong_kind(item.location(), "a string", item.kind()));
                if (*language != "C" && *language != "CXX") {
                    return std::unexpected(error_at(
                        item.location(),
                        "unsupported generated project language `" + *language + "`"
                    ));
                }
                if (std::ranges::find(result.languages, *language) == result.languages.end())
                    result.languages.push_back(*language);
            }
            if (result.languages.empty()) {
                return std::unexpected(error_at(
                    options.location_of("languages"),
                    "generated project languages cannot be empty"
                ));
            }
        }

        auto target_result = options.optional_table("target");
        if (!target_result)
            return std::unexpected(target_result.error());
        if (*target_result) {
            auto target = read_target(package.name, std::move(**target_result));
            if (!target)
                return std::unexpected(target.error());
            result.targets.push_back(std::move(*target));
        }

        auto targets_result = options.optional_table("targets");
        if (!targets_result)
            return std::unexpected(targets_result.error());
        if (*target_result && *targets_result) {
            return std::unexpected(error_at(
                options.location_of("targets"),
                "`cmake.target` and `cmake.targets` cannot be used together"
            ));
        }
        if (*targets_result) {
            TableReader targets = std::move(**targets_result);
            for (const TableEntry& entry: targets.entries()) {
                const std::string path = join_config_path(targets.path(), entry.key);
                if (!is_valid_identifier(entry.key)) {
                    SourceLocation location = entry.value.location();
                    location.config_path = path;
                    return std::unexpected(error_at(
                        std::move(location),
                        "`" + entry.key + "` is not a valid CMake target name"
                    ));
                }
                auto table = TableReader::bind(entry.value, path);
                if (!table)
                    return std::unexpected(table.error());
                auto target = read_target(entry.key, std::move(*table));
                if (!target)
                    return std::unexpected(target.error());
                result.targets.push_back(std::move(*target));
            }
            targets.take_all();
        }

        auto tests_result = options.optional_table("tests");
        if (!tests_result)
            return std::unexpected(tests_result.error());
        if (*tests_result) {
            TableReader tests = std::move(**tests_result);
            for (const TableEntry& entry: tests.entries()) {
                const std::string path = join_config_path(tests.path(), entry.key);
                auto table = TableReader::bind(entry.value, path);
                if (!table)
                    return std::unexpected(table.error());
                auto test = read_test(entry.key, std::move(*table));
                if (!test)
                    return std::unexpected(test.error());
                if (std::ranges::none_of(result.targets, [&](const TargetOptions& target) {
                        return target.name == test->target;
                    })) {
                    return std::unexpected(error_at(
                        entry.value.location(),
                        "test `" + entry.key + "` references unknown target `"
                            + test->target + "`"
                    ));
                }
                result.tests.push_back(std::move(*test));
            }
            tests.take_all();
        }

        auto dependencies_result = options.optional_table("dependencies");
        if (!dependencies_result)
            return std::unexpected(dependencies_result.error());
        if (*dependencies_result) {
            TableReader dependencies = std::move(**dependencies_result);
            for (const TableEntry& entry: dependencies.entries()) {
                const std::string* mode_name = entry.value.as_string();
                SourceLocation location = entry.value.location();
                location.config_path = join_config_path(dependencies.path(), entry.key);
                if (!mode_name)
                    return std::unexpected(wrong_kind(std::move(location), "a string", entry.value.kind()));

                const auto dependency = std::ranges::find_if(
                    package.dependencies,
                    [&](const PackageId id) { return graph[id].name == entry.key; }
                );
                if (dependency == package.dependencies.end()) {
                    return std::unexpected(error_at(
                        std::move(location),
                        "`" + entry.key + "` is not a dependency of `" + package.name + "`"
                    ));
                }
                if (graph[*dependency].kind != PackageKind::managed
                    || graph[*dependency].resolver != "cmake") {
                    return std::unexpected(error_at(
                        std::move(location),
                        "CMake integration can only be selected for a managed CMake dependency"
                    ));
                }

                DependencyMode mode;
                if (*mode_name == "add-subdirectory") {
                    mode = DependencyMode::add_subdirectory;
                } else if (*mode_name == "find-package") {
                    mode = DependencyMode::find_package;
                } else {
                    return std::unexpected(error_at(
                        std::move(location),
                        "unknown CMake dependency mode `" + *mode_name
                            + "`; expected `add-subdirectory` or `find-package`"
                    ));
                }
                result.dependencies.push_back({*dependency, mode});
            }
            dependencies.take_all();
        }

        auto finished = options.finish();
        if (!finished)
            return std::unexpected(finished.error());
        return result;
    }

    Result<BuildOptions> read_build_options(const Value* settings) {
        BuildOptions result;
        if (!settings)
            return result;

        auto options_result = TableReader::bind(*settings);
        if (!options_result)
            return std::unexpected(options_result.error());
        TableReader options = std::move(*options_result);

        auto generator = options.optional_string("generator");
        if (!generator)
            return std::unexpected(generator.error());
        result.generator = std::move(*generator);

        auto c_compiler = options.optional_string("c-compiler");
        if (!c_compiler)
            return std::unexpected(c_compiler.error());
        result.c_compiler = std::move(*c_compiler);

        auto cxx_compiler = options.optional_string("cxx-compiler");
        if (!cxx_compiler)
            return std::unexpected(cxx_compiler.error());
        result.cxx_compiler = std::move(*cxx_compiler);

        const SourceLocation toolchain_location = options.location_of("toolchain");
        auto toolchain = options.optional_string("toolchain");
        if (!toolchain)
            return std::unexpected(toolchain.error());
        if (*toolchain) {
            std::filesystem::path path = **toolchain;
            if (path.is_relative() && !toolchain_location.source.empty())
                path = std::filesystem::path(toolchain_location.source).parent_path() / path;
            result.toolchain = std::move(path);
        }

        auto arguments = string_array(options, "arguments");
        if (!arguments)
            return std::unexpected(arguments.error());
        result.arguments = std::move(*arguments);

        auto finished = options.finish();
        if (!finished)
            return std::unexpected(finished.error());
        return result;
    }

    DependencyMode dependency_mode(const Options& options, const PackageId dependency) {
        const auto selected = std::ranges::find_if(
            options.dependencies,
            [&](const DependencyOption& option) { return option.package == dependency; }
        );
        return selected == options.dependencies.end()
            ? DependencyMode::add_subdirectory
            : selected->mode;
    }

    std::string generate_project(const PackageNode& package, const Options& options) {
        std::string output = std::string(generated_marker) + "\n"
            + "cmake_minimum_required(VERSION 3.20)\n"
            + "project(" + package.name + " LANGUAGES";
        for (const std::string& language: options.languages)
            output += " " + language;
        output += ")\n\n";

        for (const TargetOptions& target: options.targets) {
            switch (target.type) {
                case TargetType::executable:
                    output += "add_executable(" + target.name;
                    break;
                case TargetType::static_library:
                    output += "add_library(" + target.name + " STATIC";
                    break;
                case TargetType::shared_library:
                    output += "add_library(" + target.name + " SHARED";
                    break;
                case TargetType::interface_library:
                    output += "add_library(" + target.name + " INTERFACE";
                    break;
            }
            const std::vector<std::string> sources = project_paths(options, target.sources);
            if (sources.empty()) {
                output += ")\n\n";
            } else {
                output += "\n";
                for (const std::string& source: sources)
                    output += "    " + quote(source) + "\n";
                output += ")\n\n";
            }

            const bool interface_target = target.type == TargetType::interface_library;
            const bool executable = target.type == TargetType::executable;
            emit_values(
                output,
                "target_include_directories",
                target.name,
                interface_target ? "INTERFACE" : "PRIVATE",
                project_paths(options, target.include_directories)
            );
            emit_values(
                output,
                "target_include_directories",
                target.name,
                interface_target ? "INTERFACE" : "PUBLIC",
                project_paths(options, target.public_include_directories)
            );
            emit_values(
                output,
                "target_include_directories",
                target.name,
                interface_target ? "SYSTEM INTERFACE" : "SYSTEM PRIVATE",
                project_paths(options, target.system_include_directories)
            );
            emit_values(
                output,
                "target_include_directories",
                target.name,
                interface_target ? "SYSTEM INTERFACE" : "SYSTEM PUBLIC",
                project_paths(options, target.public_system_include_directories)
            );
            emit_values(
                output,
                "target_link_libraries",
                target.name,
                interface_target ? "INTERFACE" : "PRIVATE",
                target.link_libraries
            );
            emit_values(
                output,
                "target_link_libraries",
                target.name,
                interface_target ? "INTERFACE" : "PUBLIC",
                target.public_link_libraries
            );
            emit_values(
                output,
                "target_compile_definitions",
                target.name,
                interface_target ? "INTERFACE" : "PRIVATE",
                target.compile_definitions
            );
            emit_values(
                output,
                "target_compile_definitions",
                target.name,
                interface_target ? "INTERFACE" : "PUBLIC",
                target.public_compile_definitions
            );
            emit_values(
                output,
                "target_compile_options",
                target.name,
                interface_target ? "INTERFACE" : "PRIVATE",
                target.compile_options
            );
            emit_values(
                output,
                "target_compile_options",
                target.name,
                interface_target ? "INTERFACE" : "PUBLIC",
                target.public_compile_options
            );

            if (target.cxx_standard) {
                const std::string scope = interface_target
                    ? "INTERFACE"
                    : (executable ? "PRIVATE" : "PUBLIC");
                output += "target_compile_features(" + target.name + " " + scope
                    + " cxx_std_" + std::to_string(*target.cxx_standard) + ")\n\n";
            }
        }

        if (!options.tests.empty()) {
            output += "enable_testing()\n\n";
            for (const TestOptions& test: options.tests) {
                output += "add_test(NAME " + quote(test.name) + " COMMAND " + test.target;
                for (const std::string& argument: test.arguments)
                    output += " " + quote(argument);
                output += ")\n";
            }
        }
        return output;
    }
}
