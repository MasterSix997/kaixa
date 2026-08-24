#include "configuration.hpp"
#include <kaixa/config/table_reader.hpp>
#include <kaixa/config/value_operations.hpp>
#include <kaixa/extension/registry.hpp>
#include <kaixa/model/effective_product.hpp>
#include <kaixa/model/file_set.hpp>
#include <kaixa/model/policy.hpp>

#include <algorithm>
#include <array>
#include <iterator>
#include <string_view>
#include <utility>

namespace kaixa::plugin::cmake::detail {
    namespace {
        Diagnostic wrong_kind(SourceLocation location, const std::string_view expected, const ValueKind found) {
            return wrong_value_kind(std::move(location), expected, found);
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
                return std::unexpected(error_at(target.location_of("type"), "unknown target type `" + *type + "`")
                        .add_note("expected `executable`, `static-library`, `shared-library` or `interface-library`"));
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
                return std::unexpected(error_at(target.location_of("sources"), "a compiled target requires at least one source"));
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

            bool discover_enabled = false;
            if (const Value* discover = test.take("discover")) {
                const bool* enabled = discover->as_boolean();
                if (!enabled) {
                    return std::unexpected(wrong_kind(discover->location(), "a boolean", discover->kind()));
                }
                discover_enabled = *enabled;
            }

            result.adapter = discover_enabled
                ? TestAdapterInfo{"kaixa", TestAdapterPurpose::test, {}, {}, {"--kaixa-test-list"}, "--kaixa-test-run", {}, {}, true}
                : TestAdapterInfo{"executable", TestAdapterPurpose::test};

            return result;
        }

        Result<TableReader> indexed_table(const Value& value, const std::string_view collection, const std::size_t index) {
            return TableReader::bind(value, std::string(collection) + "." + std::to_string(index));
        }

        Result<std::vector<std::string>> product_definitions(const Value& value, const std::filesystem::path& source_root) {
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
                    result.push_back(entry.key + "=" + (source_root / std::filesystem::path(*text)).lexically_normal().generic_string());
                } else {
                    return std::unexpected(error_at(entry.value.location(), "definition `" + entry.key + "` has an unsupported value"));
                }
            }
            return result;
        }

        bool escapes_destination(const std::filesystem::path& destination) {
            if (destination.empty() || destination.is_absolute())
                return true;

            const std::filesystem::path normalized = destination.lexically_normal();
            return normalized.empty() || *normalized.begin() == "..";
        }

        Result<void> append_runtime_file(
            TargetOptions& target,
            const std::filesystem::path& source_root,
            const std::filesystem::path& declared_source,
            const std::filesystem::path& destination,
            const bool optional,
            const SourceLocation& location
        ) {
            if (escapes_destination(destination)) {
                return std::unexpected(
                    error_at(location, "runtime destination must be a non-empty relative path inside the executable directory")
                );
            }

            const std::filesystem::path source = declared_source.is_absolute() ? declared_source.lexically_normal()
                                                                               : (source_root / declared_source).lexically_normal();
            std::error_code failure;
            const bool exists = std::filesystem::exists(source, failure);
            if (failure)
                return std::unexpected(error_at(location, "cannot inspect runtime file `" + source.string() + "`: " + failure.message()));

            if (!exists) {
                if (optional)
                    return {};

                return std::unexpected(error_at(location, "runtime file does not exist: " + source.string()));
            }

            const bool directory = std::filesystem::is_directory(source, failure);
            if (failure)
                return std::unexpected(error_at(location, "cannot inspect runtime file `" + source.string() + "`: " + failure.message()));

            const auto collision = std::ranges::find(target.runtime_files, destination, &TargetOptions::RuntimeFile::destination);
            if (collision != target.runtime_files.end() && collision->source != source) {
                return std::unexpected(
                    error_at(location, "runtime destination `" + destination.generic_string() + "` is provided by more than one source")
                );
            }
            if (collision == target.runtime_files.end())
                target.runtime_files.push_back({source, destination.lexically_normal(), directory});

            return {};
        }

        std::filesystem::path installed_header_path(const std::filesystem::path& header, const std::vector<std::string>& public_includes) {
            std::filesystem::path selected = header;
            std::size_t selected_depth = 0;
            for (const std::string& include: public_includes) {
                const std::filesystem::path root = std::filesystem::path(include).lexically_normal();
                if (root.is_absolute() || include.starts_with("$<"))
                    continue;

                const std::filesystem::path relative = header.lexically_relative(root);
                if (relative.empty() || *relative.begin() == "..")
                    continue;

                const std::size_t depth = static_cast<std::size_t>(std::ranges::distance(root));
                if (depth >= selected_depth) {
                    selected = relative;
                    selected_depth = depth;
                }
            }
            return selected.lexically_normal();
        }

        Result<void> collect_runtime_files(
            const Graph& graph,
            const PackageId package_id,
            const ProductRealizationContext& realization,
            std::vector<bool>& visited,
            TargetOptions& destination
        ) {
            if (visited[package_id.index])
                return {};

            visited[package_id.index] = true;
            const PackageNode& package = graph[package_id];
            for (const PackageId dependency: package.dependencies) {
                auto collected = collect_runtime_files(graph, dependency, realization, visited, destination);
                if (!collected)
                    return std::unexpected(collected.error());
            }
            if (package.kind == PackageKind::managed) {
                auto effective = realize_package(graph, package_id, realization);
                if (!effective)
                    return std::unexpected(effective.error());

                for (const EffectiveProduct& product: effective->products) {
                    for (const std::filesystem::path& runtime_file: product.runtime_files.files) {
                        auto appended = append_runtime_file(
                            destination,
                            package.directory,
                            runtime_file,
                            runtime_file.filename(),
                            false,
                            product.runtime_files.location
                        );
                        if (!appended)
                            return std::unexpected(appended.error());
                    }
                }
                return {};
            }
            if (!package.descriptor)
                return {};

            const Value* declared = package.descriptor->find("runtime-files");
            if (!declared)
                return {};

            const std::vector<Value>* entries = declared->as_array();
            if (!entries)
                return std::unexpected(wrong_kind(declared->location(), "an array of runtime file paths", declared->kind()));

            FileSet files;
            files.location = declared->location();
            for (const Value& entry: *entries) {
                const std::string* path = entry.as_string();
                if (!path)
                    return std::unexpected(wrong_kind(entry.location(), "a runtime file path", entry.kind()));

                files.include.push_back(*path);
            }
            auto expanded = expand_file_set(files, package.directory, package.directory, true);
            if (!expanded)
                return std::unexpected(expanded.error());

            for (const std::filesystem::path& runtime_file: *expanded) {
                auto appended = append_runtime_file(
                    destination,
                    package.directory,
                    runtime_file,
                    runtime_file.filename(),
                    false,
                    declared->location()
                );
                if (!appended)
                    return std::unexpected(appended.error());
            }
            return {};
        }

        Result<void> apply_policy(TargetOptions& target, const EffectivePolicy& policy, const std::filesystem::path& source_root) {
            if (const PolicySetting* cxx = policy.find("cxx")) {
                const std::int64_t floor = *cxx->value.as_integer();
                if (!target.cxx_standard || *target.cxx_standard < floor)
                    target.cxx_standard = floor;
            }

            if (const PolicySetting* runtime = policy.find("msvc-runtime")) {
                const std::string& value = *runtime->value.as_string();
                target.msvc_runtime = value == "static" ? MsvcRuntime::static_runtime : MsvcRuntime::dynamic_runtime;
            }

            if (const PolicySetting* warnings = policy.find("warnings")) {
                const std::string& level = *warnings->value.as_string();
                if (level != "off" && level != "default") {
                    target.compile_options.push_back("$<$<CXX_COMPILER_ID:MSVC>:/W4>");
                    target.compile_options.push_back("$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>");
                    target.compile_options.push_back("$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wextra>");
                }
                if (level == "pedantic") {
                    target.compile_options.push_back("$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wpedantic>");
                    target.compile_options.push_back("$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wconversion>");
                    target.compile_options.push_back("$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wshadow>");
                } else if (level != "off" && level != "default" && level != "strict") {
                    return std::unexpected(error_at(warnings->location, "unknown warning policy `" + level + "`"));
                }
            }

            if (const PolicySetting* errors = policy.find("warnings-as-errors"); errors && *errors->value.as_boolean()) {
                target.compile_options.push_back("$<$<CXX_COMPILER_ID:MSVC>:/WX>");
                target.compile_options.push_back("$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Werror>");
            }

            if (const PolicySetting* exceptions = policy.find("exceptions")) {
                if (*exceptions->value.as_boolean()) {
                    target.compile_options.push_back("$<$<CXX_COMPILER_ID:MSVC>:/EHsc>");
                    target.compile_options.push_back("$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-fexceptions>");
                } else {
                    target.compile_options.push_back("$<$<CXX_COMPILER_ID:MSVC>:/EHs-c->");
                    target.compile_options.push_back("$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-fno-exceptions>");
                    target.compile_definitions.push_back("$<$<CXX_COMPILER_ID:MSVC>:_HAS_EXCEPTIONS=0>");
                }
            }

            if (const PolicySetting* rtti = policy.find("rtti")) {
                if (*rtti->value.as_boolean()) {
                    target.compile_options.push_back("$<$<CXX_COMPILER_ID:MSVC>:/GR>");
                    target.compile_options.push_back("$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-frtti>");
                } else {
                    target.compile_options.push_back("$<$<CXX_COMPILER_ID:MSVC>:/GR->");
                    target.compile_options.push_back("$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-fno-rtti>");
                }
            }

            if (const PolicySetting* sanitizers = policy.find("sanitizers")) {
                std::string value;
                for (const Value& sanitizer: *sanitizers->value.as_array()) {
                    if (!value.empty())
                        value += ',';

                    value += *sanitizer.as_string();
                }
                if (!value.empty()) {
                    const std::string option = "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-fsanitize=" + value + ">";
                    target.compile_options.push_back(option);
                    target.link_options.push_back(option);
                }
            }

            if (const PolicySetting* headers = policy.find("precompiled-headers")) {
                for (const Value& header: *headers->value.as_array())
                    target.precompiled_headers.push_back(*header.as_string());
            }

            if (const PolicySetting* defines = policy.find("defines")) {
                auto values = product_definitions(defines->value, source_root);
                if (!values)
                    return std::unexpected(values.error());

                target.compile_definitions.insert(
                    target.compile_definitions.end(),
                    std::make_move_iterator(values->begin()),
                    std::make_move_iterator(values->end())
                );
            }
            return {};
        }

        Result<TargetOptions> read_effective_product(
            const EffectiveProduct& product,
            const std::optional<std::int64_t> default_standard,
            const std::filesystem::path& source_root
        ) {
            TargetOptions result;
            result.name = product.name;
            result.cxx_standard = default_standard;
            switch (product.type) {
            case EffectiveProductType::executable: result.type = TargetType::executable; break;
            case EffectiveProductType::static_library: result.type = TargetType::static_library; break;
            case EffectiveProductType::shared_library: result.type = TargetType::shared_library; break;
            case EffectiveProductType::interface_library: result.type = TargetType::interface_library; break;
            }

            result.sources.reserve(product.sources.files.size());
            for (const std::filesystem::path& source: product.sources.files)
                result.sources.push_back(source.generic_string());

            for (const std::filesystem::path& source: product.dependency_source_files)
                result.sources.push_back(source.generic_string());

            if (product.type != EffectiveProductType::interface_library) {
                for (const std::filesystem::path& header: product.headers.files)
                    result.sources.push_back(header.generic_string());

                for (const std::filesystem::path& header: product.public_headers.files)
                    result.sources.push_back(header.generic_string());
            }
            std::ranges::sort(result.sources);
            result.sources.erase(std::ranges::unique(result.sources).begin(), result.sources.end());
            result.include_directories = product.include_directories;
            result.public_include_directories = product.public_include_directories;
            result.system_include_directories = product.system_include_directories;
            result.public_system_include_directories = product.public_system_include_directories;

            for (const std::filesystem::path& header: product.public_headers.files) {
                result.install_headers.push_back(
                    {(source_root / header).lexically_normal(), installed_header_path(header, product.public_include_directories)}
                );
            }
            for (const std::filesystem::path& runtime_file: product.runtime_files.files) {
                auto appended = append_runtime_file(
                    result,
                    source_root,
                    runtime_file,
                    runtime_file.filename(),
                    false,
                    product.runtime_files.location
                );
                if (!appended)
                    return std::unexpected(appended.error());
            }

            Value private_definitions = Value::table(product.definitions, product.location);
            auto definitions = product_definitions(private_definitions, source_root);
            if (!definitions)
                return std::unexpected(definitions.error());

            result.compile_definitions = std::move(*definitions);

            Value public_definitions = Value::table(product.public_definitions, product.location);
            auto exported_definitions = product_definitions(public_definitions, source_root);
            if (!exported_definitions)
                return std::unexpected(exported_definitions.error());

            result.public_compile_definitions = std::move(*exported_definitions);

            if (result.type != TargetType::interface_library && result.sources.empty()) {
                return std::unexpected(error_at(product.location, "a compiled product requires at least one source"));
            }
            if (result.type == TargetType::interface_library
                && (!result.sources.empty()
                    || !result.include_directories.empty()
                    || !result.system_include_directories.empty()
                    || !result.compile_definitions.empty())) {
                return std::unexpected(error_at(product.location, "an interface product cannot have private product properties"));
            }
            result.install = true;
            return result;
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

            constexpr std::array path_fields{std::string_view("include-directories"),
                std::string_view("public-include-directories"),
                std::string_view("system-include-directories"),
                std::string_view("public-system-include-directories")};
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
                        Value::string(absolute.lexically_relative(project_root).lexically_normal().generic_string(), value.location())
                    );
                }
                entry.value = Value::array(std::move(normalized), entry.value.location());
            }

            for (const std::string_view reserved: {"name", "type", "sources", "source-excludes"}) {
                if (std::ranges::any_of(entries, [&](const TableEntry& entry) { return entry.key == reserved; })) {
                    return std::unexpected(error_at(
                        declared.location,
                        "`" + std::string(reserved) + "` is defined by the package target and cannot appear in its CMake options"
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

        Result<void> read_project_configuration(
            TableReader& options,
            Options& result,
            const PackageNode& package,
            const EffectivePolicy& package_policy,
            std::optional<std::int64_t>& default_standard
        ) {
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
            } else if (const PolicySetting* declared = package_policy.find("msvc-runtime")) {
                const std::string& name = *declared->value.as_string();
                if (name == "static")
                    result.msvc_runtime = MsvcRuntime::static_runtime;
                else if (name == "dynamic")
                    result.msvc_runtime = MsvcRuntime::dynamic_runtime;
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

            if (const Value* languages_value = options.take("languages")) {
                const std::vector<Value>* languages = languages_value->as_array();
                if (!languages) {
                    return std::unexpected(wrong_kind(options.location_of("languages"), "an array", languages_value->kind()));
                }
                result.languages.clear();
                for (const Value& item: *languages) {
                    const std::string* language = item.as_string();
                    if (!language)
                        return std::unexpected(wrong_kind(item.location(), "a string", item.kind()));

                    if (*language != "C" && *language != "CXX") {
                        return std::unexpected(error_at(item.location(), "unsupported generated project language `" + *language + "`"));
                    }
                    if (std::ranges::find(result.languages, *language) == result.languages.end())
                        result.languages.push_back(*language);
                }
                if (result.languages.empty()) {
                    return std::unexpected(error_at(options.location_of("languages"), "generated project languages cannot be empty"));
                }
            }

            auto configured_standard = optional_integer(options, "cxx-standard");
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
            return {};
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
            if (!package.manifest->products.empty() && (direct_target || target_value || legacy_targets_value)) {
                return std::unexpected(error_at(
                    package.manifest->products.front().location,
                    "product declarations cannot be combined with legacy CMake targets"
                ));
            }

            const auto append_target = [&](std::string name, TableReader& table) -> Result<void> {
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

                auto finished = table.finish();
                if (!finished)
                    return std::unexpected(finished.error());

                result.targets.push_back(std::move(*target));
                return {};
            };

            if (direct_target) {
                auto target = read_target(package.name, options, default_standard, result.source, result.source);
                if (!target)
                    return std::unexpected(target.error());

                auto applied_policy = apply_policy(*target, package_policy, result.source);
                if (!applied_policy)
                    return std::unexpected(applied_policy.error());

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
                    return std::unexpected(wrong_kind(options.location_of("target"), "an array of target tables", target_value->kind()));
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
            return {};
        }

        Result<void> read_declared_tests(TableReader& options, Options& result) {
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

        Result<void> read_dependency_modes(TableReader& options, Options& result, const Graph& graph, const PackageNode& package) {
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
                if (graph[*dependency].kind != PackageKind::managed || graph[*dependency].resolver != "cmake") {
                    return std::unexpected(
                        error_at(std::move(location), "CMake integration can only be selected for a managed CMake dependency")
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

        Result<void> append_effective_product(
            Options& result,
            const EffectivePackage& effective_package,
            const Graph& graph,
            const PackageNode& package,
            const EffectivePolicy& package_policy,
            const std::optional<std::int64_t> default_standard
        ) {
            if (effective_package.products.size() > 1) {
                return std::unexpected(
                    error_at(effective_package.products[1].location, "the CMake resolver currently requires one product per package")
                );
            }
            if (effective_package.products.empty())
                return {};

            auto product = read_effective_product(effective_package.products.front(), default_standard, result.source);
            if (!product)
                return std::unexpected(product.error());

            auto applied_policy = apply_policy(*product, package_policy, result.source);
            if (!applied_policy)
                return std::unexpected(applied_policy.error());

            for (const PackageId dependency: package.dependencies) {
                const PackageNode& target = graph[dependency];
                const auto binding = std::ranges::find_if(package.manifest->dependencies, [&](const DependencyBinding& candidate) {
                    return candidate.request.package == target.name;
                });
                if (binding != package.manifest->dependencies.end() && binding->visibility == DependencyVisibility::public_dependency)
                    product->public_link_libraries.push_back(target.name);
                else
                    product->link_libraries.push_back(target.name);
            }
            result.targets.push_back(std::move(*product));
            return {};
        }

        struct AssociatedTargetContext {
            const Graph& graph;
            const ExtensionRegistry& registry;
            const PackageNode& package;
            const EffectivePackage& effective_package;
            const PolicyContext& policy_context;
            const EffectivePolicy* policy_override;
            std::string_view configured_context;
            std::optional<std::int64_t> default_standard;
        };

        Result<void> append_associated_targets(Options& result, const AssociatedTargetContext& context) {
            for (const EffectiveTarget& effective_target: context.effective_package.targets) {
                if (effective_target.availability == TargetAvailability::skipped)
                    continue;

                const PackageTarget& declared = effective_target.target;
                if (!declared.name) {
                    return std::unexpected(error_at(declared.location, "package target was not normalized before CMake interpretation"));
                }
                if (std::ranges::any_of(result.targets, [&](const TargetOptions& target) { return target.name == *declared.name; })) {
                    return std::unexpected(error_at(declared.location, "duplicate CMake target `" + *declared.name + "`"));
                }

                auto target = read_package_target(declared, context.default_standard, result.source);
                if (!target)
                    return std::unexpected(target.error());

                EffectivePolicy target_policy;
                if (context.policy_override && context.configured_context == *declared.name) {
                    target_policy = *context.policy_override;
                } else {
                    std::vector<Value> layers = context.package.policy_layers;
                    if (declared.policy)
                        layers.push_back(*declared.policy);

                    auto resolved = resolve_policy_layers(layers, context.package.active_features, context.policy_context);
                    if (!resolved)
                        return std::unexpected(resolved.error());

                    target_policy = std::move(*resolved);
                }

                auto applied_policy = apply_policy(*target, target_policy, result.source);
                if (!applied_policy)
                    return std::unexpected(applied_policy.error());

                result.policy_fingerprint += ':' + policy_fingerprint(target_policy);
                target->default_build = false;
                if (std::ranges::any_of(context.effective_package.products, [](const EffectiveProduct& product) {
                        return product.type != EffectiveProductType::executable;
                    })) {
                    target->link_libraries.push_back(context.package.name);
                }
                const auto dependencies = std::ranges::find(
                    context.package.target_dependencies,
                    *declared.name,
                    &PackageTargetDependencies::target
                );
                if (dependencies != context.package.target_dependencies.end()) {
                    for (const PackageId dependency: dependencies->packages)
                        target->link_libraries.push_back(context.graph[dependency].name);
                }
                result.targets.push_back(std::move(*target));

                if (declared.kind != PackageTargetKind::test && declared.kind != PackageTargetKind::benchmark)
                    continue;

                TestAdapterInfo adapter;
                if (declared.adapter) {
                    adapter = *declared.adapter;
                } else {
                    const std::string_view framework = declared.framework ? std::string_view(*declared.framework)
                                                                          : default_test_adapter(declared.kind, declared.discover);
                    auto resolved = test_adapter(context.registry, framework, declared.kind, declared.location);
                    if (!resolved)
                        return std::unexpected(resolved.error());

                    adapter = std::move(*resolved);
                }

                if (!adapter.main_product.empty())
                    result.targets.back().link_libraries.push_back(adapter.main_product);

                result.tests.push_back(
                    {declared.display_name.value_or(*declared.name), *declared.name, declared.arguments, std::move(adapter)}
                );
            }
            return {};
        }

        Result<void> inherit_runtime_files(
            Options& result,
            const Graph& graph,
            const PackageNode& package,
            const ProductRealizationContext& realization
        ) {
            TargetOptions inherited_runtime;
            std::vector<bool> visited(graph.size(), false);
            visited[package.id.index] = true;
            for (const PackageId dependency: package.dependencies) {
                auto collected = collect_runtime_files(graph, dependency, realization, visited, inherited_runtime);
                if (!collected)
                    return std::unexpected(collected.error());
            }
            for (const PackageTargetDependencies& dependencies: package.target_dependencies) {
                for (const PackageId dependency: dependencies.packages) {
                    auto collected = collect_runtime_files(graph, dependency, realization, visited, inherited_runtime);
                    if (!collected)
                        return std::unexpected(collected.error());
                }
            }
            for (TargetOptions& target: result.targets) {
                if (target.type != TargetType::executable)
                    continue;

                for (const TargetOptions::RuntimeFile& runtime_file: inherited_runtime.runtime_files) {
                    auto appended = append_runtime_file(
                        target,
                        {},
                        runtime_file.source,
                        runtime_file.destination,
                        false,
                        package.manifest->location
                    );
                    if (!appended)
                        return std::unexpected(appended.error());
                }
            }
            return {};
        }
    }

    Result<Options> read_options(
        const Graph& graph,
        const ExtensionRegistry& registry,
        const PackageNode& package,
        const ProductRealizationContext& realization,
        const EffectivePolicy* policy_override,
        const std::string_view configured_context
    ) {
        Options result;
        result.source = package.directory;
        result.languages = {"CXX"};
        if (!package.manifest)
            return result;

        const PolicyContext policy_context{realization.profile, realization.target_os};
        EffectivePolicy resolved_package_policy;
        if (policy_override) {
            resolved_package_policy = *policy_override;
        } else {
            auto package_policy = resolve_policy_layers(package.policy_layers, package.active_features, policy_context);
            if (!package_policy)
                return std::unexpected(package_policy.error());

            resolved_package_policy = std::move(*package_policy);
        }
        const EffectivePolicy& package_policy = resolved_package_policy;
        result.policy_fingerprint = policy_fingerprint(package_policy);

        const Value empty_options = Value::table({});
        const Value& resolver_options = package.manifest->resolver_options ? *package.manifest->resolver_options : empty_options;
        auto options_result = TableReader::bind(resolver_options, "cmake");
        if (!options_result)
            return std::unexpected(options_result.error());

        TableReader options = std::move(*options_result);

        std::optional<std::int64_t> default_standard;
        auto project_configuration = read_project_configuration(options, result, package, package_policy, default_standard);
        if (!project_configuration)
            return std::unexpected(project_configuration.error());

        auto declared_targets = read_declared_targets(options, result, package, package_policy, default_standard);
        if (!declared_targets)
            return std::unexpected(declared_targets.error());

        auto effective_package = realize_package(graph, package.id, realization);
        if (!effective_package)
            return std::unexpected(effective_package.error());

        auto product = append_effective_product(result, *effective_package, graph, package, package_policy, default_standard);
        if (!product)
            return std::unexpected(product.error());

        auto declared_tests = read_declared_tests(options, result);
        if (!declared_tests)
            return std::unexpected(declared_tests.error());

        const AssociatedTargetContext target_context{graph,
            registry,
            package,
            *effective_package,
            policy_context,
            policy_override,
            configured_context,
            default_standard};
        auto associated_targets = append_associated_targets(result, target_context);
        if (!associated_targets)
            return std::unexpected(associated_targets.error());

        auto runtime_files = inherit_runtime_files(result, graph, package, realization);
        if (!runtime_files)
            return std::unexpected(runtime_files.error());

        auto dependency_modes = read_dependency_modes(options, result, graph, package);
        if (!dependency_modes)
            return std::unexpected(dependency_modes.error());

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

        result.configure_arguments.insert(result.configure_arguments.end(), configure_arguments->begin(), configure_arguments->end());

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

}
