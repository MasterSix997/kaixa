#include "configuration.hpp"

#include <generation/project.hpp>
#include <model/target_options.hpp>
#include <schema/project_options.hpp>

#include <kaixa/config/table_reader.hpp>
#include <kaixa/config/value_operations.hpp>
#include <kaixa/extension/registry.hpp>
#include <kaixa/model/effective_product.hpp>
#include <kaixa/model/file_set.hpp>
#include <kaixa/model/policy.hpp>
#include <model/native_product.hpp>

#include <algorithm>
#include <array>
#include <iterator>
#include <string_view>
#include <utility>

namespace kaixa::plugin::cmake::detail {
    namespace {
        Result<std::vector<std::string>> descriptor_strings(const PackageNode& package, const std::string_view key) {
            if (!package.descriptor())
                return std::vector<std::string>{};

            const Value* value = package.descriptor()->find(key);
            if (!value)
                return std::vector<std::string>{};

            const std::vector<Value>* entries = value->as_array();
            if (!entries)
                return std::unexpected(wrong_kind(value->location(), "an array of strings", value->kind()));

            std::vector<std::string> result;
            result.reserve(entries->size());
            for (const Value& entry: *entries) {
                const std::string* text = entry.as_string();
                if (!text)
                    return std::unexpected(wrong_kind(entry.location(), "a string", entry.kind()));

                if (text->empty())
                    return std::unexpected(error_at(entry.location(), "package descriptor paths cannot be empty"));

                result.push_back(*text);
            }
            return result;
        }

        Result<std::filesystem::path> opaque_package_root(const PackageNode& package) {
            if (!package.directory.empty())
                return package.directory;

            if (package.descriptor()) {
                const Value* source = package.descriptor()->find("source");
                const Value* driver = source ? source->find("driver") : nullptr;
                const Value* path = source ? source->find("path") : nullptr;
                const std::string* driver_name = driver ? driver->as_string() : nullptr;
                const std::string* declared_path = path ? path->as_string() : nullptr;
                if (driver_name && *driver_name == "path" && declared_path) {
                    std::filesystem::path resolved = *declared_path;
                    if (resolved.is_relative() && !path->location().source.empty())
                        resolved = std::filesystem::path(path->location().source).parent_path() / resolved;

                    return std::filesystem::absolute(resolved).lexically_normal();
                }
            }
            return std::unexpected(error("opaque package `" + package.name + "` has no materialized directory"));
        }

        bool library_is_path(const std::string_view library) {
            const std::filesystem::path path{library};
            return path.has_parent_path() || path.has_extension();
        }

        Result<std::string> dependency_product_name(const PackageNode& package, const DependencyBinding* binding = nullptr) {
            if (!package.descriptor())
                return package.name;

            const Value* products = package.descriptor()->find("products");
            if (!products)
                return package.name;

            std::optional<std::string> selected;
            if (binding) {
                for (const std::string& feature: binding->request.features) {
                    const Value* feature_product = products->find(feature);
                    if (!feature_product)
                        continue;

                    const std::string* name = feature_product->as_string();
                    if (!name || name->empty()) {
                        return std::unexpected(error_at(feature_product->location(), "feature package product must be a non-empty string"));
                    }
                    if (selected && *selected != *name) {
                        return std::unexpected(error_at(
                            feature_product->location(),
                            "dependency requests features that select different products for package `" + package.name + "`"
                        ));
                    }
                    selected = *name;
                }
            }
            if (selected)
                return std::move(*selected);

            const Value* default_product = products->find("default");
            if (!default_product)
                return std::unexpected(error_at(products->location(), "package products require a `default` entry"));

            const std::string* name = default_product->as_string();
            if (!name || name->empty())
                return std::unexpected(error_at(default_product->location(), "default package product must be a non-empty string"));

            return *name;
        }

        Result<std::optional<std::string>> descriptor_find_package(const PackageNode& package) {
            if (!package.descriptor())
                return std::nullopt;

            const Value* consumer = package.descriptor()->find("consumer");
            if (!consumer)
                return std::nullopt;
            if (!consumer->as_table())
                return std::unexpected(wrong_kind(consumer->location(), "a consumer table", consumer->kind()));

            const Value* mode = consumer->find("mode");
            const std::string* mode_name = mode ? mode->as_string() : nullptr;
            if (!mode || !mode_name || *mode_name != "find-package")
                return std::nullopt;

            const Value* declared = consumer->find("package");
            const std::string* name = declared ? declared->as_string() : nullptr;
            if (!name || name->empty()) {
                return std::unexpected(error_at(
                    declared ? declared->location() : consumer->location(),
                    "find-package consumer requires a non-empty `package` string"
                ));
            }
            return *name;
        }

        Result<void> apply_opaque_dependency(TargetOptions& product, const PackageNode& dependency, const DependencyVisibility visibility) {
            auto includes = descriptor_strings(dependency, "include");
            auto system_includes = descriptor_strings(dependency, "system-include");
            auto libraries = descriptor_strings(dependency, "libraries");
            auto system_libraries = descriptor_strings(dependency, "system-libraries");
            if (!includes)
                return std::unexpected(includes.error());

            if (!system_includes)
                return std::unexpected(system_includes.error());

            if (!libraries)
                return std::unexpected(libraries.error());

            if (!system_libraries)
                return std::unexpected(system_libraries.error());

            const bool requires_root = !includes->empty() || !system_includes->empty() || std::ranges::any_of(*libraries, library_is_path);
            std::filesystem::path root;
            if (requires_root) {
                auto resolved = opaque_package_root(dependency);
                if (!resolved)
                    return std::unexpected(resolved.error());

                root = std::move(*resolved);
            }

            std::vector<std::string>& target_includes = visibility == DependencyVisibility::public_dependency
                ? product.public_include_directories
                : product.include_directories;
            std::vector<std::string>& target_system_includes = visibility == DependencyVisibility::public_dependency
                ? product.public_system_include_directories
                : product.system_include_directories;
            std::vector<std::string>& target_libraries = visibility == DependencyVisibility::public_dependency
                ? product.public_link_libraries
                : product.link_libraries;

            for (const std::string& include: *includes)
                target_includes.push_back((root / include).lexically_normal().generic_string());

            for (const std::string& include: *system_includes)
                target_system_includes.push_back((root / include).lexically_normal().generic_string());

            for (const std::string& library: *libraries) {
                target_libraries.push_back(library_is_path(library) ? (root / library).lexically_normal().generic_string() : library);
            }
            target_libraries.insert(target_libraries.end(), system_libraries->begin(), system_libraries->end());
            return {};
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

        struct HeaderInstallPath {
            std::filesystem::path path;
            bool under_public_include = false;
        };

        HeaderInstallPath installed_header_path(const std::filesystem::path& header, const std::vector<std::string>& public_includes) {
            HeaderInstallPath selected{header, false};
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
                    selected = {relative, true};
                    selected_depth = depth;
                }
            }
            selected.path = selected.path.lexically_normal();
            return selected;
        }

        Result<void> collect_runtime_files(
            const Graph& graph,
            const PackageId package_id,
            const ProductRealizationContext& realization,
            std::vector<bool>& visited,
            TargetOptions& destination,
            ConfigurationCache& cache
        ) {
            if (visited[package_id.index])
                return {};

            visited[package_id.index] = true;
            const PackageNode& package = graph[package_id];
            for (const PackageId dependency: package.dependencies) {
                auto collected = collect_runtime_files(graph, dependency, realization, visited, destination, cache);
                if (!collected)
                    return std::unexpected(collected.error());
            }
            if (package.is_managed()) {
                if (cache.effective_packages.size() < graph.size())
                    cache.effective_packages.resize(graph.size());

                std::optional<EffectivePackage>& stored = cache.effective_packages[package_id.index];
                if (!stored) {
                    auto effective = realize_package(graph, package_id, realization, &cache.files);
                    if (!effective)
                        return std::unexpected(effective.error());

                    stored = std::move(*effective);
                }

                for (const EffectiveProduct& product: stored->products) {
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
            if (!package.descriptor())
                return {};

            const Value* declared = package.descriptor()->find("runtime-files");
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
            auto expanded = expand_file_set(files, package.directory, package.directory, true, &cache.files);
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

        Result<TargetOptions> read_effective_product(
            const EffectiveProduct& product,
            const NativeProductOptions& native,
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

            for (const std::filesystem::path& source: native.dependency_source_files)
                result.sources.push_back(source.generic_string());

            for (const std::filesystem::path& header: native.headers.files)
                result.sources.push_back(header.generic_string());

            for (const std::filesystem::path& header: native.public_headers.files)
                result.sources.push_back(header.generic_string());
            std::ranges::sort(result.sources);
            result.sources.erase(std::ranges::unique(result.sources).begin(), result.sources.end());
            result.include_directories = native.include_directories;
            result.public_include_directories = native.public_include_directories;
            result.system_include_directories = native.system_include_directories;
            result.public_system_include_directories = native.public_system_include_directories;
            result.link_libraries = native.system_libraries;

            for (const std::filesystem::path& header: native.headers.files) {
                const HeaderInstallPath destination = installed_header_path(header, native.public_include_directories);
                if (destination.under_public_include)
                    result.install_headers.push_back({(source_root / header).lexically_normal(), destination.path});
            }
            for (const std::filesystem::path& header: native.public_headers.files) {
                const HeaderInstallPath destination = installed_header_path(header, native.public_include_directories);
                result.install_headers.push_back({(source_root / header).lexically_normal(), destination.path});
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

            Value private_definitions = Value::table(native.definitions, product.location);
            auto definitions = product_definitions(private_definitions, source_root);
            if (!definitions)
                return std::unexpected(definitions.error());

            result.compile_definitions = std::move(*definitions);

            Value public_definitions = Value::table(native.public_definitions, product.location);
            auto exported_definitions = product_definitions(public_definitions, source_root);
            if (!exported_definitions)
                return std::unexpected(exported_definitions.error());

            result.public_compile_definitions = std::move(*exported_definitions);

            if (result.type != TargetType::interface_library && result.sources.empty()) {
                return std::unexpected(error_at(product.location, "compiled product `" + result.name + "` requires at least one source"));
            }
            if (result.type == TargetType::interface_library
                && (!product.sources.files.empty()
                    || !native.dependency_source_files.empty()
                    || !result.include_directories.empty()
                    || !result.system_include_directories.empty()
                    || !result.compile_definitions.empty())) {
                return std::unexpected(
                    error_at(product.location, "an interface product cannot have compilation sources or private usage requirements")
                );
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

            std::vector<std::string> declared_includes;
            std::vector<std::string> declared_system_includes;
            std::vector<TableEntry> declared_definitions;
            std::vector<std::string> declared_system_libraries;
            auto take_string_array = [&](const TableEntry& entry, std::vector<std::string>& output) -> Result<void> {
                const std::vector<Value>* values = entry.value.as_array();
                if (!values)
                    return std::unexpected(error_at(entry.value.location(), "`" + entry.key + "` must be a string array"));

                for (const Value& value: *values) {
                    const std::string* text = value.as_string();
                    if (!text)
                        return std::unexpected(error_at(value.location(), "`" + entry.key + "` must contain strings"));

                    output.push_back(*text);
                }
                return {};
            };
            for (auto entry = entries.begin(); entry != entries.end();) {
                std::vector<std::string>* strings = nullptr;
                if (entry->key == "include")
                    strings = &declared_includes;
                else if (entry->key == "system-include")
                    strings = &declared_system_includes;
                else if (entry->key == "system-libraries")
                    strings = &declared_system_libraries;

                if (strings != nullptr) {
                    auto taken = take_string_array(*entry, *strings);
                    if (!taken)
                        return std::unexpected(taken.error());

                    entry = entries.erase(entry);
                    continue;
                }
                if (entry->key == "defines") {
                    const std::vector<TableEntry>* definitions = entry->value.as_table();
                    if (!definitions)
                        return std::unexpected(error_at(entry->value.location(), "target definitions must be a table"));

                    declared_definitions = *definitions;
                    entry = entries.erase(entry);
                    continue;
                }
                ++entry;
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

            const std::filesystem::path target_root = declared.source.parent_path();
            for (const std::string& include: declared_includes) {
                target->include_directories.push_back(
                    (target_root / include).lexically_normal().lexically_relative(project_root).generic_string()
                );
            }
            for (const std::string& include: declared_system_includes) {
                target->system_include_directories.push_back(
                    (target_root / include).lexically_normal().lexically_relative(project_root).generic_string()
                );
            }
            Value definitions = Value::table(declared_definitions, declared.location);
            auto normalized_definitions = product_definitions(definitions, target_root);
            if (!normalized_definitions)
                return std::unexpected(normalized_definitions.error());

            target->compile_definitions.insert(
                target->compile_definitions.end(),
                std::make_move_iterator(normalized_definitions->begin()),
                std::make_move_iterator(normalized_definitions->end())
            );
            target->link_libraries.insert(target->link_libraries.end(), declared_system_libraries.begin(), declared_system_libraries.end());

            auto finished = table.finish();
            if (!finished)
                return std::unexpected(finished.error());

            return target;
        }

        Result<void> append_effective_product(
            Options& result,
            const EffectivePackage& effective_package,
            const Graph& graph,
            const PackageNode& package,
            const EffectivePolicy& package_policy,
            const std::optional<std::int64_t> default_standard,
            FileCatalog* files
        ) {
            if (effective_package.products.size() > 1) {
                return std::unexpected(
                    error_at(effective_package.products[1].location, "the CMake resolver currently requires one product per package")
                );
            }
            if (effective_package.products.empty())
                return {};

            auto native = read_native_product_options(effective_package.products.front(), graph, package, files);
            if (!native)
                return std::unexpected(native.error());

            auto product = read_effective_product(effective_package.products.front(), *native, default_standard, result.source);
            if (!product)
                return std::unexpected(product.error());

            auto applied_policy = apply_policy(*product, package_policy, result.source);
            if (!applied_policy)
                return std::unexpected(applied_policy.error());

            for (const PackageId dependency: package.dependencies) {
                const PackageNode& target = graph[dependency];
                const auto binding = std::ranges::find_if(package.manifest()->dependencies, [&](const DependencyBinding& candidate) {
                    return candidate.request.package == target.name;
                });
                const DependencyVisibility visibility = binding != package.manifest()->dependencies.end()
                    ? binding->visibility
                    : DependencyVisibility::private_dependency;
                if (target.is_opaque()) {
                    auto applied = apply_opaque_dependency(*product, target, visibility);
                    if (!applied)
                        return std::unexpected(applied.error());

                    auto find_package = descriptor_find_package(target);
                    if (!find_package)
                        return std::unexpected(find_package.error());
                    if (*find_package) {
                        if (std::ranges::find(result.find_packages, **find_package) == result.find_packages.end())
                            result.find_packages.push_back(**find_package);

                        if (visibility == DependencyVisibility::public_dependency
                            && std::ranges::find(result.export_dependencies, **find_package) == result.export_dependencies.end()) {
                            result.export_dependencies.push_back(**find_package);
                        }

                        auto linked_product = dependency_product_name(
                            target,
                            binding != package.manifest()->dependencies.end() ? &*binding : nullptr
                        );
                        if (!linked_product)
                            return std::unexpected(linked_product.error());

                        if (visibility == DependencyVisibility::public_dependency)
                            product->public_link_libraries.push_back(std::move(*linked_product));
                        else
                            product->link_libraries.push_back(std::move(*linked_product));
                    }

                    continue;
                }
                auto linked_product = dependency_product_name(
                    target,
                    binding != package.manifest()->dependencies.end() ? &*binding : nullptr
                );
                if (!linked_product)
                    return std::unexpected(linked_product.error());

                if (visibility == DependencyVisibility::public_dependency) {
                    if (target.is_managed()) {
                        product->public_link_libraries.push_back("$<BUILD_INTERFACE:" + *linked_product + ">");
                        product->public_link_libraries.push_back("$<INSTALL_INTERFACE:" + target.name + "::" + *linked_product + ">");
                        if (std::ranges::find(result.export_dependencies, target.name) == result.export_dependencies.end())
                            result.export_dependencies.push_back(target.name);
                    } else {
                        product->public_link_libraries.push_back(std::move(*linked_product));
                    }
                } else {
                    product->link_libraries.push_back(std::move(*linked_product));
                }
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

                    auto resolved = resolve_policy_layers(
                        layers,
                        context.package.active_features,
                        context.policy_context,
                        context.registry.policy_schema()
                    );
                    if (!resolved)
                        return std::unexpected(resolved.error());

                    target_policy = std::move(*resolved);
                }

                auto applied_policy = apply_policy(*target, target_policy, result.source);
                if (!applied_policy)
                    return std::unexpected(applied_policy.error());

                result.policy_fingerprint += ':' + policy_fingerprint(target_policy);
                target->default_build = declared.install;
                target->install = declared.install;
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
                    for (const PackageId dependency: dependencies->packages) {
                        const PackageNode& dependency_package = context.graph[dependency];
                        const auto binding = std::ranges::find_if(
                            context.package.manifest()->dependencies,
                            [&](const DependencyBinding& candidate) { return candidate.request.package == dependency_package.name; }
                        );
                        auto linked_product = dependency_product_name(
                            dependency_package,
                            binding != context.package.manifest()->dependencies.end() ? &*binding : nullptr
                        );
                        if (!linked_product)
                            return std::unexpected(linked_product.error());

                        target->link_libraries.push_back(std::move(*linked_product));
                    }
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

                std::vector<std::string>& link_libraries = result.targets.back().link_libraries;
                if (!adapter.main_product.empty() && std::ranges::find(link_libraries, adapter.main_product) == link_libraries.end()) {
                    link_libraries.push_back(adapter.main_product);
                }

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
            const ProductRealizationContext& realization,
            ConfigurationCache& cache
        ) {
            TargetOptions inherited_runtime;
            std::vector<bool> visited(graph.size(), false);
            visited[package.id.index] = true;
            for (const PackageId dependency: package.dependencies) {
                auto collected = collect_runtime_files(graph, dependency, realization, visited, inherited_runtime, cache);
                if (!collected)
                    return std::unexpected(collected.error());
            }
            for (const PackageTargetDependencies& dependencies: package.target_dependencies) {
                for (const PackageId dependency: dependencies.packages) {
                    auto collected = collect_runtime_files(graph, dependency, realization, visited, inherited_runtime, cache);
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
                        package.manifest()->location
                    );
                    if (!appended)
                        return std::unexpected(appended.error());
                }
            }
            return {};
        }
    }

    Result<const Options*> read_cached_options(
        const Graph& graph,
        const ExtensionRegistry& registry,
        const PackageNode& package,
        const ProductRealizationContext& realization,
        const EffectivePolicy* policy_override,
        const std::string_view configured_context,
        ConfigurationCache& cache
    ) {
        if (cache.effective_packages.size() < graph.size())
            cache.effective_packages.resize(graph.size());

        ConfigurationCacheKey cache_key{package.id, std::string(configured_context), std::nullopt};
        if (policy_override)
            cache_key.policy = policy_fingerprint(*policy_override);

        const auto cached = cache.options.find(cache_key);
        if (cached != cache.options.end())
            return &cached->second;

        Options result;
        result.source = package.directory;
        result.languages = {"CXX"};
        for (const PackageNode& candidate: graph.nodes()) {
            if (candidate.source && !candidate.directory.empty())
                result.portable_source_roots.push_back({candidate.directory, source_variable(candidate.id)});
        }
        if (!package.manifest())
            return &cache.options.emplace(std::move(cache_key), std::move(result)).first->second;

        const PolicyContext policy_context{realization.profile, realization.target_os};
        EffectivePolicy resolved_package_policy;
        if (policy_override) {
            resolved_package_policy = *policy_override;
        } else {
            auto package_policy = resolve_policy_layers(
                package.policy_layers,
                package.active_features,
                policy_context,
                registry.policy_schema()
            );
            if (!package_policy)
                return std::unexpected(package_policy.error());

            resolved_package_policy = std::move(*package_policy);
        }
        const EffectivePolicy& package_policy = resolved_package_policy;
        result.policy_fingerprint = policy_fingerprint(package_policy);

        const Value empty_options = Value::table({});
        const Value& resolver_options = package.manifest()->resolver_options ? *package.manifest()->resolver_options : empty_options;
        auto options_result = TableReader::bind(resolver_options, "cmake");
        if (!options_result)
            return std::unexpected(options_result.error());

        TableReader options = std::move(*options_result);

        auto schema = read_declared_project(options, result, package, package_policy);
        if (!schema)
            return std::unexpected(schema.error());

        const std::optional<std::int64_t> default_standard = schema->default_standard;

        std::optional<EffectivePackage>& stored_effective = cache.effective_packages[package.id.index];
        if (!stored_effective) {
            auto effective_package = realize_package(graph, package.id, realization, &cache.files);
            if (!effective_package)
                return std::unexpected(effective_package.error());

            stored_effective = std::move(*effective_package);
        }

        auto product = append_effective_product(result, *stored_effective, graph, package, package_policy, default_standard, &cache.files);
        if (!product)
            return std::unexpected(product.error());

        auto declared_tests = read_declared_tests(options, result);
        if (!declared_tests)
            return std::unexpected(declared_tests.error());

        const AssociatedTargetContext target_context{graph,
            registry,
            package,
            *stored_effective,
            policy_context,
            policy_override,
            configured_context,
            default_standard};
        auto associated_targets = append_associated_targets(result, target_context);
        if (!associated_targets)
            return std::unexpected(associated_targets.error());

        auto runtime_files = inherit_runtime_files(result, graph, package, realization, cache);
        if (!runtime_files)
            return std::unexpected(runtime_files.error());

        auto dependency_modes = read_dependency_modes(options, result, graph, package);
        if (!dependency_modes)
            return std::unexpected(dependency_modes.error());

        auto finished = options.finish();
        if (!finished)
            return std::unexpected(finished.error());

        return &cache.options.emplace(std::move(cache_key), std::move(result)).first->second;
    }

    std::string source_variable(const PackageId package) {
        return "_kaixa_package_" + std::to_string(package.index) + "_source";
    }

    DependencyMode dependency_mode(const Options& options, const PackageId dependency) {
        const auto selected = std::ranges::find_if(options.dependencies, [&](const DependencyOption& option) {
            return option.package == dependency;
        });
        return selected == options.dependencies.end() ? DependencyMode::add_subdirectory : selected->mode;
    }

}
