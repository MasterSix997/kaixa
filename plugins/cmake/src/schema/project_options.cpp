#include "project_options.hpp"

#include <model/target_options.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kaixa::plugin::cmake::detail {
    namespace {
        Result<TestOptions> read_test(std::string name, TableReader& test) {
            TestOptions result;
            result.name = std::move(name);

            auto target = test.string("target");
            if (!target)
                return std::unexpected(target.error());

            result.target = std::move(*target);

            auto arguments = test.string_array("arguments");
            if (!arguments)
                return std::unexpected(arguments.error());

            result.arguments = std::move(*arguments);

            auto discover = test.boolean("discover");
            if (!discover)
                return std::unexpected(discover.error());

            result.adapter = *discover
                ? TestAdapterInfo{"kaixa", TestAdapterPurpose::test, {}, {}, {"--kaixa-test-list"}, "--kaixa-test-run", {}, {}, true}
                : TestAdapterInfo{"executable", TestAdapterPurpose::test};

            return result;
        }

        Result<TableReader> indexed_table(const Value& value, const std::string_view collection, const std::size_t index) {
            return TableReader::bind(value, std::string(collection) + "." + std::to_string(index));
        }

        Result<void> read_generation_mode(TableReader& options, Options& result) {
            auto generation = options.optional_string("generation");
            if (!generation)
                return std::unexpected(generation.error());

            if (!*generation)
                return {};

            if (**generation == "export")
                result.generation = GenerationMode::export_project;
            else if (**generation == "state")
                result.generation = GenerationMode::state;
            else {
                return std::unexpected(error_at(
                    options.location_of("generation"),
                    "unknown CMake generation mode `" + **generation + "`; expected `export` or `state`"
                ));
            }
            return {};
        }

        Result<void> read_msvc_runtime(TableReader& options, Options& result, const EffectivePolicy& package_policy) {
            auto runtime = options.optional_string("msvc-runtime");
            if (!runtime)
                return std::unexpected(runtime.error());

            const std::string* name = nullptr;
            if (*runtime)
                name = &**runtime;
            else if (const PolicySetting* declared = package_policy.find("msvc-runtime"))
                name = declared->value.as_string();

            if (!name)
                return {};

            if (*name == "static")
                result.msvc_runtime = MsvcRuntime::static_runtime;
            else if (*name == "dynamic")
                result.msvc_runtime = MsvcRuntime::dynamic_runtime;
            else if (*runtime) {
                return std::unexpected(
                    error_at(options.location_of("msvc-runtime"), "unknown MSVC runtime `" + *name + "`; expected `static` or `dynamic`")
                );
            }
            return {};
        }

        Result<void> read_output_paths(TableReader& options, Options& result) {
            auto output_result = options.optional_table("output");
            if (!output_result)
                return std::unexpected(output_result.error());

            if (!*output_result)
                return {};

            TableReader output = std::move(**output_result);
            const auto read_path = [&](const std::string_view key, std::optional<std::filesystem::path>& destination) -> Result<void> {
                auto value = output.optional_string(key);
                if (!value)
                    return std::unexpected(value.error());

                if (*value)
                    destination = **value;

                return {};
            };
            for (
                auto [key, destination]: {std::pair{std::string_view{"runtime"}, &result.runtime_output},
                    std::pair{std::string_view{"library"}, &result.library_output},
                    std::pair{std::string_view{"archive"}, &result.archive_output}}
            ) {
                auto read = read_path(key, *destination);
                if (!read)
                    return std::unexpected(read.error());
            }
            return output.finish();
        }

        Result<void> read_project_languages(TableReader& options, Options& result) {
            auto languages = options.optional_string_array("languages");
            if (!languages)
                return std::unexpected(languages.error());

            if (!*languages)
                return {};

            if ((*languages)->empty()) {
                return std::unexpected(error_at(options.location_of("languages"), "generated project languages cannot be empty"));
            }
            result.languages.clear();
            for (std::string& language: **languages) {
                if (language != "C" && language != "CXX") {
                    return std::unexpected(
                        error_at(options.location_of("languages"), "unsupported generated project language `" + language + "`")
                    );
                }
                if (std::ranges::find(result.languages, language) == result.languages.end())
                    result.languages.push_back(std::move(language));
            }
            return {};
        }

        Result<void> read_project_configuration(
            TableReader& options,
            Options& result,
            const EffectivePolicy& package_policy,
            std::optional<std::int64_t>& default_standard
        ) {
            auto source = options.optional_string("source");
            if (!source)
                return std::unexpected(source.error());

            if (*source)
                result.source /= **source;

            auto generation = read_generation_mode(options, result);
            if (!generation)
                return std::unexpected(generation.error());

            auto runtime = read_msvc_runtime(options, result, package_policy);
            if (!runtime)
                return std::unexpected(runtime.error());

            auto outputs = read_output_paths(options, result);
            if (!outputs)
                return std::unexpected(outputs.error());

            auto languages = read_project_languages(options, result);
            if (!languages)
                return std::unexpected(languages.error());

            auto configured_standard = options.optional_integer("cxx-standard");
            if (!configured_standard)
                return std::unexpected(configured_standard.error());

            default_standard = *configured_standard;
            if (!default_standard) {
                if (const PolicySetting* cxx = package_policy.find("cxx"))
                    default_standard = *cxx->value.as_integer();
            }
            if (default_standard && *default_standard <= 0) {
                return std::unexpected(error_at(options.location_of("cxx-standard"), "C++ standard must be positive"));
            }
            result.cxx_standard = default_standard;
            return {};
        }

        struct DeclaredTargetInput {
            std::string name;
            TableReader table;
        };

        Result<std::vector<DeclaredTargetInput>> current_target_inputs(const Value& value, const std::string_view default_name) {
            std::vector<DeclaredTargetInput> result;
            if (const std::vector<Value>* targets = value.as_array()) {
                result.reserve(targets->size());
                for (std::size_t index = 0; index < targets->size(); ++index) {
                    auto table = indexed_table((*targets)[index], "cmake.target", index);
                    if (!table)
                        return std::unexpected(table.error());

                    auto name = table->string("name");
                    if (!name)
                        return std::unexpected(name.error());

                    result.push_back({std::move(*name), std::move(*table)});
                }
                return result;
            }
            if (!value.is_table())
                return std::unexpected(wrong_kind(value.location(), "an array of target tables", value.kind()));

            auto table = TableReader::bind(value, "cmake.target");
            if (!table)
                return std::unexpected(table.error());

            result.push_back({std::string(default_name), std::move(*table)});
            return result;
        }

        Result<std::vector<DeclaredTargetInput>> legacy_target_inputs(const Value& value) {
            auto targets_result = TableReader::bind(value, "cmake.targets");
            if (!targets_result)
                return std::unexpected(targets_result.error());

            TableReader targets = std::move(*targets_result);
            std::vector<DeclaredTargetInput> result;
            result.reserve(targets.entries().size());
            for (const TableEntry& entry: targets.entries()) {
                auto table = TableReader::bind(entry.value, join_config_path(targets.path(), entry.key));
                if (!table)
                    return std::unexpected(table.error());

                result.push_back({entry.key, std::move(*table)});
            }
            return result;
        }

        Result<void> read_declared_targets(
            TableReader& options,
            Options& result,
            const PackageNode& package,
            const EffectivePolicy& package_policy,
            const std::optional<std::int64_t> default_standard
        ) {
            const bool direct_target = std::ranges::any_of(options.entries(), [](const TableEntry& entry) { return entry.key == "type"; });
            const Value* target_value = options.take("target");
            const Value* legacy_targets_value = options.take("targets");
            if (direct_target && (target_value || legacy_targets_value)) {
                return std::unexpected(
                    error_at(options.location_of("type"), "target fields in `cmake` cannot be combined with target entries")
                );
            }
            if (target_value && legacy_targets_value) {
                return std::unexpected(
                    error_at(options.location_of("targets"), "`cmake.target` and `cmake.targets` cannot be used together")
                );
            }
            if (!package.manifest()->products.empty() && (direct_target || target_value || legacy_targets_value)) {
                return std::unexpected(error_at(
                    package.manifest()->products.front().location,
                    "product declarations cannot be combined with legacy CMake targets"
                ));
            }

            const auto append_target = [&](std::string name, TableReader& table, const bool finish = true) -> Result<void> {
                if (!is_valid_identifier(name)) {
                    return std::unexpected(error_at(table.location_of("name"), "`" + name + "` is not a valid CMake target name"));
                }
                if (std::ranges::any_of(result.targets, [&](const TargetOptions& target) { return target.name == name; })) {
                    return std::unexpected(error_at(table.location_of("name"), "duplicate CMake target `" + name + "`"));
                }

                auto target = read_target(std::move(name), table, default_standard, result.source, result.source);
                if (!target)
                    return std::unexpected(target.error());

                auto applied_policy = apply_policy(*target, package_policy, result.source);
                if (!applied_policy)
                    return std::unexpected(applied_policy.error());

                if (finish) {
                    auto finished = table.finish();
                    if (!finished)
                        return std::unexpected(finished.error());
                }

                result.targets.push_back(std::move(*target));
                return {};
            };

            if (direct_target) {
                auto appended = append_target(package.name, options, false);
                if (!appended)
                    return std::unexpected(appended.error());
                return {};
            }

            Result<std::vector<DeclaredTargetInput>> inputs = std::vector<DeclaredTargetInput>{};
            if (target_value)
                inputs = current_target_inputs(*target_value, package.name);
            else if (legacy_targets_value)
                inputs = legacy_target_inputs(*legacy_targets_value);

            if (!inputs)
                return std::unexpected(inputs.error());

            for (DeclaredTargetInput& input: *inputs) {
                auto appended = append_target(std::move(input.name), input.table);
                if (!appended)
                    return std::unexpected(appended.error());
            }
            return {};
        }

        Result<void> read_declared_tests_table(TableReader& options, Options& result) {
            const auto append_test = [&](std::string name, TableReader& table) -> Result<void> {
                auto test = read_test(std::move(name), table);
                if (!test)
                    return std::unexpected(test.error());

                auto finished = table.finish();
                if (!finished)
                    return std::unexpected(finished.error());

                if (std::ranges::none_of(result.targets, [&](const TargetOptions& target) { return target.name == test->target; })) {
                    return std::unexpected(
                        error_at(table.location_of("target"), "test `" + test->name + "` references unknown target `" + test->target + "`")
                    );
                }
                result.tests.push_back(std::move(*test));
                return {};
            };

            if (const Value* tests_value = options.take("test")) {
                const std::vector<Value>* tests = tests_value->as_array();
                if (!tests) {
                    return std::unexpected(wrong_kind(options.location_of("test"), "an array of test tables", tests_value->kind()));
                }
                for (std::size_t index = 0; index < tests->size(); ++index) {
                    auto table_result = indexed_table((*tests)[index], "cmake.test", index);
                    if (!table_result)
                        return std::unexpected(table_result.error());

                    TableReader table = std::move(*table_result);
                    auto name = table.string("name");
                    if (!name)
                        return std::unexpected(name.error());

                    auto appended = append_test(std::move(*name), table);
                    if (!appended)
                        return std::unexpected(appended.error());
                }
            }

            if (const Value* legacy_tests_value = options.take("tests")) {
                auto tests_result = TableReader::bind(*legacy_tests_value, "cmake.tests");
                if (!tests_result)
                    return std::unexpected(tests_result.error());

                TableReader tests = std::move(*tests_result);
                for (const TableEntry& entry: tests.entries()) {
                    auto table_result = TableReader::bind(entry.value, join_config_path(tests.path(), entry.key));
                    if (!table_result)
                        return std::unexpected(table_result.error());

                    TableReader table = std::move(*table_result);
                    auto appended = append_test(entry.key, table);
                    if (!appended)
                        return std::unexpected(appended.error());
                }
                tests.take_all();
            }
            return {};
        }

        Result<void> read_dependency_modes_table(TableReader& options, Options& result, const Graph& graph, const PackageNode& package) {
            auto dependencies_result = options.optional_table("dependencies");
            if (!dependencies_result)
                return std::unexpected(dependencies_result.error());

            if (!*dependencies_result)
                return {};

            TableReader dependencies = std::move(**dependencies_result);
            for (const TableEntry& entry: dependencies.entries()) {
                const std::string* mode_name = entry.value.as_string();
                SourceLocation location = entry.value.location();
                location.config_path = join_config_path(dependencies.path(), entry.key);
                if (!mode_name)
                    return std::unexpected(wrong_kind(std::move(location), "a string", entry.value.kind()));

                const auto dependency = std::ranges::find_if(package.dependencies, [&](const PackageId id) {
                    return graph[id].name == entry.key;
                });
                if (dependency == package.dependencies.end()) {
                    return std::unexpected(
                        error_at(std::move(location), "`" + entry.key + "` is not a dependency of `" + package.name + "`")
                    );
                }
                if (!graph[*dependency].has_build_semantics() || graph[*dependency].resolver != "cmake") {
                    return std::unexpected(
                        error_at(std::move(location), "CMake integration requires a managed or adopted CMake dependency")
                    );
                }

                DependencyMode mode;
                if (*mode_name == "add-subdirectory") {
                    mode = DependencyMode::add_subdirectory;
                } else if (*mode_name == "find-package") {
                    mode = DependencyMode::find_package;
                } else {
                    return std::unexpected(error_at(
                        std::move(location),
                        "unknown CMake dependency mode `" + *mode_name + "`; expected `add-subdirectory` or `find-package`"
                    ));
                }
                result.dependencies.push_back({*dependency, mode});
            }
            dependencies.take_all();
            return {};
        }
    }

    Result<TargetOptions> read_target(
        std::string name,
        TableReader& target,
        const std::optional<std::int64_t> default_standard,
        const std::filesystem::path& source_root,
        const std::filesystem::path& output_root
    ) {
        TargetOptions result;
        result.name = std::move(name);
        result.cxx_standard = default_standard;

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
            return std::unexpected(error_at(target.location_of("type"), "unknown target type `" + *type + "`")
                    .add_note("expected `executable`, `static-library`, `shared-library` or `interface-library`"));
        }

        auto sources = target.string_array("sources");
        if (!sources)
            return std::unexpected(sources.error());

        auto source_excludes = target.string_array("source-excludes");
        if (!source_excludes)
            return std::unexpected(source_excludes.error());

        FileSet source_files{std::move(*sources), std::move(*source_excludes), {}, target.location_of("sources")};
        auto expanded_sources = expand_file_set(source_files, source_root, output_root);
        if (!expanded_sources)
            return std::unexpected(expanded_sources.error());

        for (const std::filesystem::path& source: *expanded_sources)
            result.sources.push_back(source.generic_string());

        auto includes = target.string_array("include-directories");
        if (!includes)
            return std::unexpected(includes.error());

        result.include_directories = std::move(*includes);

        auto public_includes = target.string_array("public-include-directories");
        if (!public_includes)
            return std::unexpected(public_includes.error());

        result.public_include_directories = std::move(*public_includes);

        auto system_includes = target.string_array("system-include-directories");
        if (!system_includes)
            return std::unexpected(system_includes.error());

        result.system_include_directories = std::move(*system_includes);

        auto public_system_includes = target.string_array("public-system-include-directories");
        if (!public_system_includes)
            return std::unexpected(public_system_includes.error());

        result.public_system_include_directories = std::move(*public_system_includes);

        auto links = target.string_array("link-libraries");
        if (!links)
            return std::unexpected(links.error());

        result.link_libraries = std::move(*links);

        auto public_links = target.string_array("public-link-libraries");
        if (!public_links)
            return std::unexpected(public_links.error());

        result.public_link_libraries = std::move(*public_links);

        auto definitions = target.string_array("compile-definitions");
        if (!definitions)
            return std::unexpected(definitions.error());

        result.compile_definitions = std::move(*definitions);

        auto public_definitions = target.string_array("public-compile-definitions");
        if (!public_definitions)
            return std::unexpected(public_definitions.error());

        result.public_compile_definitions = std::move(*public_definitions);

        auto compile_options = target.string_array("compile-options");
        if (!compile_options)
            return std::unexpected(compile_options.error());

        result.compile_options = std::move(*compile_options);

        auto public_compile_options = target.string_array("public-compile-options");
        if (!public_compile_options)
            return std::unexpected(public_compile_options.error());

        result.public_compile_options = std::move(*public_compile_options);

        auto standard = target.optional_integer("cxx-standard");
        if (!standard)
            return std::unexpected(standard.error());

        if (*standard)
            result.cxx_standard = *standard;

        if (result.type != TargetType::interface_library && result.sources.empty()) {
            return std::unexpected(
                error_at(target.location_of("sources"), "compiled target `" + result.name + "` requires at least one source")
            );
        }
        if (result.type == TargetType::interface_library
            && (!result.sources.empty()
                || !result.include_directories.empty()
                || !result.system_include_directories.empty()
                || !result.link_libraries.empty()
                || !result.compile_definitions.empty()
                || !result.compile_options.empty())) {
            return std::unexpected(error_at(target.location_of("type"), "an interface library cannot have private target properties"));
        }
        if (result.cxx_standard && *result.cxx_standard <= 0) {
            return std::unexpected(error_at(target.location_of("cxx-standard"), "C++ standard must be positive"));
        }

        return result;
    }

    Result<ProjectSchema> read_declared_project(
        TableReader& options,
        Options& result,
        const PackageNode& package,
        const EffectivePolicy& package_policy
    ) {
        ProjectSchema schema;
        auto configuration = read_project_configuration(options, result, package_policy, schema.default_standard);
        if (!configuration)
            return std::unexpected(configuration.error());

        auto targets = read_declared_targets(options, result, package, package_policy, schema.default_standard);
        if (!targets)
            return std::unexpected(targets.error());

        return schema;
    }

    Result<void> read_declared_tests(TableReader& options, Options& result) {
        return read_declared_tests_table(options, result);
    }

    Result<void> read_dependency_modes(TableReader& options, Options& result, const Graph& graph, const PackageNode& package) {
        return read_dependency_modes_table(options, result, graph, package);
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

        auto arguments = options.string_array("arguments");
        if (!arguments)
            return std::unexpected(arguments.error());

        result.configure_arguments = std::move(*arguments);

        auto configure_arguments = options.string_array("configure-arguments");
        if (!configure_arguments)
            return std::unexpected(configure_arguments.error());

        result.configure_arguments.insert(result.configure_arguments.end(), configure_arguments->begin(), configure_arguments->end());

        auto build_arguments = options.string_array("build-arguments");
        if (!build_arguments)
            return std::unexpected(build_arguments.error());

        result.build_arguments = std::move(*build_arguments);

        auto install_arguments = options.string_array("install-arguments");
        if (!install_arguments)
            return std::unexpected(install_arguments.error());

        result.install_arguments = std::move(*install_arguments);

        auto finished = options.finish();
        if (!finished)
            return std::unexpected(finished.error());

        return result;
    }
}
