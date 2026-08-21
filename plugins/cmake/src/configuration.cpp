#include "configuration.hpp"
#include "testing.hpp"

#include <kaixa/config/table_reader.hpp>
#include <kaixa/model/file_set.hpp>

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

namespace kaixa::plugin::cmake::detail {
    namespace {
        Diagnostic wrong_kind(SourceLocation location, const std::string_view expected, const ValueKind found) {
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
                    return std::unexpected(wrong_kind(std::move(location), "a string array element", item.kind()));
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
                return std::unexpected(
                    error_at(target.location_of("type"), "unknown target type `" + *type + "`")
                        .add_note("expected `executable`, `static-library`, `shared-library` or `interface-library`")
                );
            }

            auto sources = string_array(target, "sources");
            if (!sources)
                return std::unexpected(sources.error());

            auto source_excludes = string_array(target, "source-excludes");
            if (!source_excludes)
                return std::unexpected(source_excludes.error());

            FileSet source_files{std::move(*sources), std::move(*source_excludes), {}, target.location_of("sources")};
            auto expanded_sources = expand_file_set(source_files, source_root, output_root);
            if (!expanded_sources)
                return std::unexpected(expanded_sources.error());

            for (const std::filesystem::path& source: *expanded_sources)
                result.sources.push_back(source.generic_string());

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

            auto public_system_includes = string_array(target, "public-system-include-directories");
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
            if (*standard)
                result.cxx_standard = *standard;

            if (result.type != TargetType::interface_library && result.sources.empty()) {
                return std::unexpected(
                    error_at(target.location_of("sources"), "a compiled target requires at least one source")
                );
            }
            if (result.type == TargetType::interface_library
                && (!result.sources.empty() || !result.include_directories.empty()
                    || !result.system_include_directories.empty() || !result.link_libraries.empty()
                    || !result.compile_definitions.empty() || !result.compile_options.empty())) {
                return std::unexpected(
                    error_at(target.location_of("type"), "an interface library cannot have private target properties")
                );
            }
            if (result.cxx_standard && *result.cxx_standard <= 0) {
                return std::unexpected(error_at(target.location_of("cxx-standard"), "C++ standard must be positive"));
            }

            return result;
        }

        Result<TestOptions> read_test(std::string name, TableReader& test) {
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

            if (const Value* discover = test.take("discover")) {
                const bool* enabled = discover->as_boolean();
                if (!enabled) {
                    return std::unexpected(wrong_kind(discover->location(), "a boolean", discover->kind()));
                }
                result.discover = *enabled;
            }

            return result;
        }

        Result<TableReader>
        indexed_table(const Value& value, const std::string_view collection, const std::size_t index) {
            return TableReader::bind(value, std::string(collection) + "." + std::to_string(index));
        }

        std::string quote(const std::string_view value) {
            std::string equals;
            while (value.contains("]" + equals + "]"))
                equals += '=';
            return "[" + equals + "[" + std::string(value) + "]" + equals + "]";
        }

        std::string quoted_string(const std::string_view value) {
            std::string result = "\"";
            for (const char character: value) {
                if (character == '\\' || character == '"')
                    result.push_back('\\');
                result.push_back(character);
            }
            result.push_back('"');
            return result;
        }

        std::string output_directory(const std::filesystem::path& path) {
            const std::string value = path.generic_string();
            return path.is_absolute() ? quoted_string(value)
                                      : quoted_string("${_kaixa_output_root}/" + value + "/$<0:>");
        }

        std::string project_version(const PackageNode& package) {
            if (!package.manifest || !package.manifest->version)
                return {};

            std::string value = package.manifest->version->text;
            const std::size_t suffix = value.find_first_of("-+");
            if (suffix != std::string::npos)
                value.resize(suffix);
            if (value.empty() || value.front() == '.' || value.back() == '.')
                return {};
            if (!std::ranges::all_of(value, [](const char character) {
                    return (character >= '0' && character <= '9') || character == '.';
                })) {
                return {};
            }
            return value;
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

        std::vector<std::string> project_paths(const Options& options, const std::vector<std::string>& values) {
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

        Result<std::vector<std::string>>
        product_definitions(const Value& value, const std::filesystem::path& source_root) {
            const std::vector<TableEntry>* table = value.as_table();
            if (!table)
                return std::unexpected(wrong_kind(value.location(), "a definitions table", value.kind()));

            std::vector<std::string> result;
            result.reserve(table->size());
            for (const TableEntry& entry: *table) {
                if (const bool* boolean = entry.value.as_boolean()) {
                    result.push_back(*boolean ? entry.key : entry.key + "=0");
                } else if (const std::int64_t* integer = entry.value.as_integer()) {
                    result.push_back(entry.key + "=" + std::to_string(*integer));
                } else if (const std::string* text = entry.value.as_string()) {
                    result.push_back(entry.key + "=" + *text);
                } else if (const Value* path = entry.value.find("path")) {
                    const std::string* text = path->as_string();
                    if (!text) {
                        return std::unexpected(wrong_kind(path->location(), "a path string", path->kind()));
                    }
                    result.push_back(
                        entry.key + "="
                        + (source_root / std::filesystem::path(*text)).lexically_normal().generic_string()
                    );
                } else {
                    return std::unexpected(
                        error_at(entry.value.location(), "definition `" + entry.key + "` has an unsupported value")
                    );
                }
            }
            return result;
        }

        Result<TargetOptions> read_product(
            const ProductDeclaration& product,
            const std::string& package_name,
            const std::optional<std::int64_t> default_standard,
            const std::filesystem::path& source_root
        ) {
            auto product_result =
                TableReader::bind(product.options, product.kind == ProductDeclarationKind::library ? "lib" : "bin");
            if (!product_result)
                return std::unexpected(product_result.error());
            TableReader table = std::move(*product_result);

            std::string type = product.kind == ProductDeclarationKind::executable ? "executable" : "static-library";
            auto declared_type = table.optional_string("type");
            if (!declared_type)
                return std::unexpected(declared_type.error());
            if (*declared_type) {
                if (**declared_type == "static")
                    type = "static-library";
                else if (**declared_type == "shared")
                    type = "shared-library";
                else if (**declared_type == "interface")
                    type = "interface-library";
                else {
                    return std::unexpected(
                        error_at(table.location_of("type"), "unknown product type `" + **declared_type + "`")
                            .add_note("expected `static`, `shared` or `interface`")
                    );
                }
            }

            std::vector<TableEntry> translated;
            translated.push_back({"type", Value(type)});
            for (const auto [product_key, cmake_key]:
                 {std::pair{std::string_view{"sources"}, std::string_view{"sources"}},
                  std::pair{std::string_view{"include"}, std::string_view{"include-directories"}},
                  std::pair{std::string_view{"public-include"}, std::string_view{"public-include-directories"}},
                  std::pair{std::string_view{"system-include"}, std::string_view{"system-include-directories"}},
                  std::pair{
                      std::string_view{"public-system-include"}, std::string_view{"public-system-include-directories"}
                  }}) {
                if (const Value* value = table.take(product_key))
                    translated.push_back({std::string(cmake_key), *value});
            }

            for (const auto [product_key, cmake_key]:
                 {std::pair{std::string_view{"defines"}, std::string_view{"compile-definitions"}},
                  std::pair{std::string_view{"public-defines"}, std::string_view{"public-compile-definitions"}}}) {
                const Value* value = table.take(product_key);
                if (!value)
                    continue;
                auto definitions = product_definitions(*value, source_root);
                if (!definitions)
                    return std::unexpected(definitions.error());
                std::vector<Value> values;
                values.reserve(definitions->size());
                for (std::string& definition: *definitions)
                    values.emplace_back(std::move(definition));
                translated.push_back({std::string(cmake_key), Value::array(std::move(values), value->location())});
            }

            table.take("headers");
            table.take("public-headers");
            table.take("modules");
            table.take("dependency-sources");
            table.take("when");

            auto finished = table.finish();
            if (!finished)
                return std::unexpected(finished.error());

            Value value = Value::table(std::move(translated), product.location);
            auto translated_result = TableReader::bind(value, "cmake.product");
            if (!translated_result)
                return std::unexpected(translated_result.error());
            TableReader translated_table = std::move(*translated_result);
            auto target = read_target(package_name, translated_table, default_standard, source_root, source_root);
            if (!target)
                return std::unexpected(target.error());
            auto translated_finished = translated_table.finish();
            if (!translated_finished)
                return std::unexpected(translated_finished.error());
            return target;
        }

        Result<TargetOptions> read_package_target(
            const PackageTarget& declared,
            const std::optional<std::int64_t> default_standard,
            const std::filesystem::path& project_root
        ) {
            std::vector<TableEntry> entries;
            if (declared.resolver_options) {
                const std::vector<TableEntry>* options = declared.resolver_options->as_table();
                if (!options)
                    return std::unexpected(error_at(declared.location, "CMake target options must be a table"));

                entries = *options;
            }

            constexpr std::array path_fields{
                std::string_view("include-directories"),
                std::string_view("public-include-directories"),
                std::string_view("system-include-directories"),
                std::string_view("public-system-include-directories")
            };
            for (TableEntry& entry: entries) {
                if (std::ranges::find(path_fields, entry.key) == path_fields.end())
                    continue;

                const std::vector<Value>* values = entry.value.as_array();
                if (!values)
                    continue;

                std::vector<Value> normalized;
                normalized.reserve(values->size());
                for (const Value& value: *values) {
                    const std::string* text = value.as_string();
                    if (!text || text->starts_with("$<") || std::filesystem::path(*text).is_absolute()) {
                        normalized.push_back(value);
                        continue;
                    }

                    const std::filesystem::path absolute = declared.source.parent_path() / std::filesystem::path(*text);
                    normalized.push_back(
                        Value::string(
                            absolute.lexically_relative(project_root).lexically_normal().generic_string(),
                            value.location()
                        )
                    );
                }
                entry.value = Value::array(std::move(normalized), entry.value.location());
            }

            for (const std::string_view reserved: {"name", "type", "sources", "source-excludes"}) {
                if (std::ranges::any_of(entries, [&](const TableEntry& entry) { return entry.key == reserved; })) {
                    return std::unexpected(error_at(
                        declared.location,
                        "`" + std::string(reserved)
                            + "` is defined by the package target and cannot appear in its CMake options"
                    ));
                }
            }

            std::vector<Value> sources;
            sources.reserve(declared.sources.files.size());
            for (const std::filesystem::path& source: declared.sources.files)
                sources.emplace_back(source.generic_string());

            entries.push_back({"type", Value("executable")});
            entries.push_back({"sources", Value::array(std::move(sources))});

            Value document = Value::table(std::move(entries), declared.location);
            auto table_result = TableReader::bind(document, "cmake");
            if (!table_result)
                return std::unexpected(table_result.error());

            TableReader table = std::move(*table_result);
            auto target = read_target(*declared.name, table, default_standard, project_root, project_root);
            if (!target)
                return std::unexpected(target.error());

            auto finished = table.finish();
            if (!finished)
                return std::unexpected(finished.error());

            return target;
        }
    }

    Result<Options> read_options(const Graph& graph, const PackageNode& package) {
        Options result;
        result.source = package.directory;
        result.languages = {"CXX"};
        if (!package.manifest)
            return result;

        const Value empty_options = Value::table({});
        const Value& resolver_options =
            package.manifest->resolver_options ? *package.manifest->resolver_options : empty_options;
        auto options_result = TableReader::bind(resolver_options, "cmake");
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
                    "unknown CMake generation location `" + **generation + "`; expected `source` or `state`"
                ));
            }
        }

        auto runtime = options.optional_string("msvc-runtime");
        if (!runtime)
            return std::unexpected(runtime.error());

        if (*runtime) {
            if (**runtime == "static") {
                result.msvc_runtime = MsvcRuntime::static_runtime;
            } else if (**runtime == "dynamic") {
                result.msvc_runtime = MsvcRuntime::dynamic_runtime;
            } else {
                return std::unexpected(error_at(
                    options.location_of("msvc-runtime"),
                    "unknown MSVC runtime `" + **runtime + "`; expected `static` or `dynamic`"
                ));
            }
        } else {
            for (const Value& policy: package.policy_layers) {
                const Value* declared = policy.find("msvc-runtime");
                if (!declared)
                    continue;
                const std::string* name = declared->as_string();
                if (!name) {
                    return std::unexpected(wrong_kind(declared->location(), "an MSVC runtime name", declared->kind()));
                }
                if (*name == "static")
                    result.msvc_runtime = MsvcRuntime::static_runtime;
                else if (*name == "dynamic")
                    result.msvc_runtime = MsvcRuntime::dynamic_runtime;
                else {
                    return std::unexpected(error_at(
                        declared->location(), "unknown MSVC runtime `" + *name + "`; expected `static` or `dynamic`"
                    ));
                }
            }
        }

        auto output_result = options.optional_table("output");
        if (!output_result)
            return std::unexpected(output_result.error());
        if (*output_result) {
            TableReader output = std::move(**output_result);
            auto runtime_output = output.optional_string("runtime");
            if (!runtime_output)
                return std::unexpected(runtime_output.error());
            if (*runtime_output)
                result.runtime_output = **runtime_output;

            auto library_output = output.optional_string("library");
            if (!library_output)
                return std::unexpected(library_output.error());
            if (*library_output)
                result.library_output = **library_output;

            auto archive_output = output.optional_string("archive");
            if (!archive_output)
                return std::unexpected(archive_output.error());
            if (*archive_output)
                result.archive_output = **archive_output;

            auto finished = output.finish();
            if (!finished)
                return std::unexpected(finished.error());
        }

        const Value* languages_value = options.take("languages");
        if (languages_value) {
            const std::vector<Value>* languages = languages_value->as_array();
            if (!languages) {
                return std::unexpected(
                    wrong_kind(options.location_of("languages"), "an array", languages_value->kind())
                );
            }
            result.languages.clear();
            for (const Value& item: *languages) {
                const std::string* language = item.as_string();
                if (!language)
                    return std::unexpected(wrong_kind(item.location(), "a string", item.kind()));
                if (*language != "C" && *language != "CXX") {
                    return std::unexpected(
                        error_at(item.location(), "unsupported generated project language `" + *language + "`")
                    );
                }
                if (std::ranges::find(result.languages, *language) == result.languages.end())
                    result.languages.push_back(*language);
            }
            if (result.languages.empty()) {
                return std::unexpected(
                    error_at(options.location_of("languages"), "generated project languages cannot be empty")
                );
            }
        }

        auto default_standard = optional_integer(options, "cxx-standard");
        if (!default_standard)
            return std::unexpected(default_standard.error());
        if (!*default_standard) {
            for (const Value& policy: package.policy_layers) {
                const Value* cxx = policy.find("cxx");
                if (!cxx)
                    continue;
                const std::int64_t* standard = cxx->as_integer();
                if (!standard) {
                    return std::unexpected(wrong_kind(cxx->location(), "an integer C++ language floor", cxx->kind()));
                }
                default_standard = *standard;
            }
        }
        if (*default_standard && **default_standard <= 0) {
            return std::unexpected(error_at(options.location_of("cxx-standard"), "C++ standard must be positive"));
        }

        const bool direct_target =
            std::ranges::any_of(options.entries(), [](const TableEntry& entry) { return entry.key == "type"; });
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
        if (!package.manifest->products.empty() && (direct_target || target_value || legacy_targets_value)) {
            return std::unexpected(error_at(
                package.manifest->products.front().location,
                "product declarations cannot be combined with legacy CMake targets"
            ));
        }

        auto append_target = [&](std::string name, TableReader& table) -> Result<void> {
            if (!is_valid_identifier(name)) {
                return std::unexpected(
                    error_at(table.location_of("name"), "`" + name + "` is not a valid CMake target name")
                );
            }
            if (std::ranges::any_of(result.targets, [&](const TargetOptions& target) { return target.name == name; })) {
                return std::unexpected(error_at(table.location_of("name"), "duplicate CMake target `" + name + "`"));
            }

            auto target = read_target(std::move(name), table, *default_standard, result.source, result.source);
            if (!target)
                return std::unexpected(target.error());

            auto finished = table.finish();
            if (!finished)
                return std::unexpected(finished.error());

            result.targets.push_back(std::move(*target));
            return {};
        };

        if (direct_target) {
            auto target = read_target(package.name, options, *default_standard, result.source, result.source);
            if (!target)
                return std::unexpected(target.error());

            result.targets.push_back(std::move(*target));
        } else if (target_value) {
            if (const std::vector<Value>* targets = target_value->as_array()) {
                for (std::size_t index = 0; index < targets->size(); ++index) {
                    auto table_result = indexed_table((*targets)[index], "cmake.target", index);
                    if (!table_result)
                        return std::unexpected(table_result.error());

                    TableReader table = std::move(*table_result);
                    auto name = table.string("name");
                    if (!name)
                        return std::unexpected(name.error());

                    auto appended = append_target(std::move(*name), table);
                    if (!appended)
                        return std::unexpected(appended.error());
                }
            } else if (target_value->is_table()) {
                auto table_result = TableReader::bind(*target_value, "cmake.target");
                if (!table_result)
                    return std::unexpected(table_result.error());

                TableReader table = std::move(*table_result);
                auto appended = append_target(package.name, table);
                if (!appended)
                    return std::unexpected(appended.error());
            } else {
                return std::unexpected(
                    wrong_kind(options.location_of("target"), "an array of target tables", target_value->kind())
                );
            }
        } else if (legacy_targets_value) {
            auto targets_result = TableReader::bind(*legacy_targets_value, "cmake.targets");
            if (!targets_result)
                return std::unexpected(targets_result.error());
            TableReader targets = std::move(*targets_result);
            for (const TableEntry& entry: targets.entries()) {
                auto table_result = TableReader::bind(entry.value, join_config_path(targets.path(), entry.key));
                if (!table_result)
                    return std::unexpected(table_result.error());
                TableReader table = std::move(*table_result);
                auto appended = append_target(entry.key, table);
                if (!appended)
                    return std::unexpected(appended.error());
            }
            targets.take_all();
        }

        if (package.manifest->products.size() > 1) {
            return std::unexpected(error_at(
                package.manifest->products[1].location, "the CMake resolver currently requires one product per package"
            ));
        }
        if (!package.manifest->products.empty()) {
            auto product =
                read_product(package.manifest->products.front(), package.name, *default_standard, result.source);
            if (!product)
                return std::unexpected(product.error());

            for (const PackageId dependency: package.dependencies) {
                const PackageNode& target = graph[dependency];
                const auto binding =
                    std::ranges::find_if(package.manifest->dependencies, [&](const DependencyBinding& candidate) {
                        return candidate.request.package == target.name;
                    });
                if (binding != package.manifest->dependencies.end()
                    && binding->visibility == DependencyVisibility::public_dependency) {
                    product->public_link_libraries.push_back(target.name);
                } else {
                    product->link_libraries.push_back(target.name);
                }
            }
            result.targets.push_back(std::move(*product));
        }

        auto append_test = [&](std::string name, TableReader& table) -> Result<void> {
            auto test = read_test(std::move(name), table);
            if (!test)
                return std::unexpected(test.error());

            auto finished = table.finish();
            if (!finished)
                return std::unexpected(finished.error());
            if (std::ranges::none_of(result.targets, [&](const TargetOptions& target) {
                    return target.name == test->target;
                })) {
                return std::unexpected(error_at(
                    table.location_of("target"),
                    "test `" + test->name + "` references unknown target `" + test->target + "`"
                ));
            }
            result.tests.push_back(std::move(*test));
            return {};
        };

        if (const Value* tests_value = options.take("test")) {
            const std::vector<Value>* tests = tests_value->as_array();
            if (!tests) {
                return std::unexpected(
                    wrong_kind(options.location_of("test"), "an array of test tables", tests_value->kind())
                );
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

        for (const PackageTarget& declared: package.manifest->resolved_targets) {
            if (!declared.name) {
                return std::unexpected(
                    error_at(declared.location, "package target was not normalized before CMake interpretation")
                );
            }
            if (std::ranges::any_of(result.targets, [&](const TargetOptions& target) {
                    return target.name == *declared.name;
                })) {
                return std::unexpected(error_at(declared.location, "duplicate CMake target `" + *declared.name + "`"));
            }

            auto target = read_package_target(declared, *default_standard, result.source);
            if (!target)
                return std::unexpected(target.error());

            target->default_build = false;
            if (std::ranges::any_of(package.manifest->products, [](const ProductDeclaration& product) {
                    return product.kind == ProductDeclarationKind::library;
                })) {
                target->link_libraries.push_back(package.name);
            }
            const auto dependencies =
                std::ranges::find(package.target_dependencies, *declared.name, &PackageTargetDependencies::target);
            if (dependencies != package.target_dependencies.end()) {
                for (const PackageId dependency: dependencies->packages)
                    target->link_libraries.push_back(graph[dependency].name);
            }
            result.targets.push_back(std::move(*target));

            if (declared.kind == PackageTargetKind::test) {
                result.tests.push_back(
                    {declared.display_name.value_or(*declared.name),
                     *declared.name,
                     declared.arguments,
                     declared.discover}
                );
            }
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

                const auto dependency = std::ranges::find_if(package.dependencies, [&](const PackageId id) {
                    return graph[id].name == entry.key;
                });
                if (dependency == package.dependencies.end()) {
                    return std::unexpected(error_at(
                        std::move(location), "`" + entry.key + "` is not a dependency of `" + package.name + "`"
                    ));
                }
                if (graph[*dependency].kind != PackageKind::managed || graph[*dependency].resolver != "cmake") {
                    return std::unexpected(error_at(
                        std::move(location), "CMake integration can only be selected for a managed CMake dependency"
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
        result.configure_arguments = std::move(*arguments);

        auto configure_arguments = string_array(options, "configure-arguments");
        if (!configure_arguments)
            return std::unexpected(configure_arguments.error());
        result.configure_arguments.insert(
            result.configure_arguments.end(), configure_arguments->begin(), configure_arguments->end()
        );

        auto build_arguments = string_array(options, "build-arguments");
        if (!build_arguments)
            return std::unexpected(build_arguments.error());
        result.build_arguments = std::move(*build_arguments);

        auto install_arguments = string_array(options, "install-arguments");
        if (!install_arguments)
            return std::unexpected(install_arguments.error());
        result.install_arguments = std::move(*install_arguments);

        auto finished = options.finish();
        if (!finished)
            return std::unexpected(finished.error());
        return result;
    }

    DependencyMode dependency_mode(const Options& options, const PackageId dependency) {
        const auto selected = std::ranges::find_if(options.dependencies, [&](const DependencyOption& option) {
            return option.package == dependency;
        });
        return selected == options.dependencies.end() ? DependencyMode::add_subdirectory : selected->mode;
    }

    std::string generate_project(const PackageNode& package, const Options& options) {
        const std::string version = project_version(package);
        std::string output =
            std::string(generated_marker) + "\n" + "cmake_minimum_required(VERSION 3.20)\n" + "project(" + package.name;
        if (!version.empty())
            output += " VERSION " + version;
        output += " LANGUAGES";
        for (const std::string& language: options.languages)
            output += " " + language;
        output += ")\n\n";

        if (options.runtime_output || options.library_output || options.archive_output) {
            output += "if(PROJECT_IS_TOP_LEVEL)\n";
            output += "    if(KAIXA_OUTPUT_ROOT)\n";
            output += "        set(_kaixa_output_root \"${KAIXA_OUTPUT_ROOT}\")\n";
            output += "    else()\n";
            output += "        set(_kaixa_output_root \"${CMAKE_BINARY_DIR}\")\n";
            output += "    endif()\n";
            if (options.runtime_output) {
                output += "    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY " + output_directory(*options.runtime_output) + ")\n";
            }
            if (options.library_output) {
                output += "    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY " + output_directory(*options.library_output) + ")\n";
            }
            if (options.archive_output) {
                output += "    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY " + output_directory(*options.archive_output) + ")\n";
            }
            output += "endif()\n\n";
        }

        if (options.msvc_runtime != MsvcRuntime::default_runtime) {
            const std::string runtime =
                options.msvc_runtime == MsvcRuntime::static_runtime ? "MultiThreaded" : "MultiThreadedDLL";
            output += "set(CMAKE_MSVC_RUNTIME_LIBRARY \"" + runtime + "$<$<CONFIG:Debug>:Debug>\")\n\n";
        }

        for (const TargetOptions& target: options.targets) {
            switch (target.type) {
            case TargetType::executable: output += "add_executable(" + target.name; break;
            case TargetType::static_library: output += "add_library(" + target.name + " STATIC"; break;
            case TargetType::shared_library: output += "add_library(" + target.name + " SHARED"; break;
            case TargetType::interface_library: output += "add_library(" + target.name + " INTERFACE"; break;
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
                const std::string scope = interface_target ? "INTERFACE" : (executable ? "PRIVATE" : "PUBLIC");
                output += "target_compile_features(" + target.name + " " + scope + " cxx_std_"
                          + std::to_string(*target.cxx_standard) + ")\n\n";
                output += "set_target_properties(" + target.name + " PROPERTIES CXX_EXTENSIONS OFF)\n\n";
            }
            if (!target.default_build) {
                output += "set_target_properties(" + target.name + " PROPERTIES EXCLUDE_FROM_ALL TRUE)\n\n";
            }
        }

        generate_tests(output, options.tests);
        return output;
    }
}
