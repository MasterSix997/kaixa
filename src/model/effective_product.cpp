#include <kaixa/model/effective_product.hpp>

#include <kaixa/config/table_reader.hpp>
#include <kaixa/config/value_operations.hpp>
#include <kaixa/model/file_set.hpp>

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <utility>

namespace kaixa {
    namespace {
        Diagnostic wrong_kind(SourceLocation location, const std::string_view expected, const ValueKind found) {
            return wrong_value_kind(std::move(location), expected, found);
        }

        Value merge_product_values(const Value& base, const Value& overlay) {
            return merge_values(base, overlay, ArrayMerge::append);
        }

        Result<bool> condition_matches(const Value& condition, const PackageNode& package, const ProductRealizationContext& context) {
            const std::vector<TableEntry>* entries = condition.as_table();
            if (!entries)
                return std::unexpected(wrong_kind(condition.location(), "a condition table", condition.kind()));

            for (const TableEntry& entry: *entries) {
                std::vector<std::string> expected;
                if (const std::string* text = entry.value.as_string()) {
                    expected.push_back(*text);
                } else if (const std::vector<Value>* values = entry.value.as_array()) {
                    for (const Value& value: *values) {
                        const std::string* expected_value = value.as_string();
                        if (!expected_value) {
                            return std::unexpected(wrong_kind(value.location(), "a condition string", value.kind()));
                        }
                        expected.push_back(*expected_value);
                    }
                } else {
                    return std::unexpected(wrong_kind(entry.value.location(), "a condition string or string array", entry.value.kind()));
                }

                if (entry.key == "feature") {
                    if (std::ranges::none_of(expected, [&](const std::string& feature) {
                            return std::ranges::find(package.active_features, feature) != package.active_features.end();
                        })) {
                        return false;
                    }
                } else if (entry.key == "profile") {
                    if (std::ranges::find(expected, context.profile) == expected.end())
                        return false;

                } else if (entry.key == "target-os") {
                    const std::string& target_os = context.target_os.empty() ? host_target_os() : context.target_os;
                    if (std::ranges::find(expected, target_os) == expected.end())
                        return false;

                } else {
                    return std::unexpected(error_at(entry.value.location(), "unknown product condition `" + entry.key + "`"));
                }
            }
            return true;
        }

        Result<Value> effective_product_value(
            const ProductDeclaration& declaration,
            const PackageNode& package,
            const ProductRealizationContext& context
        ) {
            const std::vector<TableEntry>* declared = declaration.options.as_table();
            if (!declared)
                return std::unexpected(wrong_kind(declaration.location, "a product table", declaration.options.kind()));

            std::vector<TableEntry> base;
            const Value* conditions = nullptr;
            for (const TableEntry& entry: *declared) {
                if (entry.key == "when")
                    conditions = &entry.value;
                else
                    base.push_back(entry);
            }
            Value result = Value::table(std::move(base), declaration.location);
            if (!conditions)
                return result;

            const std::vector<Value>* branches = conditions->as_array();
            if (!branches)
                return std::unexpected(wrong_kind(conditions->location(), "an array of conditions", conditions->kind()));

            for (const Value& branch: *branches) {
                const std::vector<TableEntry>* entries = branch.as_table();
                if (!entries)
                    return std::unexpected(wrong_kind(branch.location(), "a conditional product table", branch.kind()));

                const Value* condition = branch.find("if");
                if (!condition)
                    return std::unexpected(error_at(branch.location(), "conditional product entry requires `if`"));

                auto matches = condition_matches(*condition, package, context);
                if (!matches)
                    return std::unexpected(matches.error());

                if (!*matches)
                    continue;

                std::vector<TableEntry> overlay;
                for (const TableEntry& entry: *entries) {
                    if (entry.key != "if")
                        overlay.push_back(entry);
                }
                result = merge_product_values(result, Value::table(std::move(overlay), branch.location()));
            }
            return result;
        }

        Result<std::vector<TableEntry>> definitions(TableReader& table, const std::string_view key) {
            const Value* value = table.take(key);
            if (!value)
                return std::vector<TableEntry>{};

            const std::vector<TableEntry>* entries = value->as_table();
            if (!entries)
                return std::unexpected(wrong_kind(value->location(), "a definitions table", value->kind()));

            return *entries;
        }

        Result<const PackageNode*> dependency_by_local_name(
            const Graph& graph,
            const PackageNode& package,
            const std::string_view local_name,
            const SourceLocation& location
        ) {
            const auto binding = std::ranges::find_if(package.manifest->dependencies, [&](const DependencyBinding& item) {
                return item.local_name() == local_name;
            });
            if (binding == package.manifest->dependencies.end()) {
                return std::unexpected(error_at(location, "unknown dependency `" + std::string(local_name) + "` in product path"));
            }
            const auto dependency = std::ranges::find_if(package.dependencies, [&](const PackageId id) {
                return graph[id].name == binding->request.package;
            });
            if (dependency == package.dependencies.end()) {
                return std::unexpected(error_at(location, "dependency `" + std::string(local_name) + "` is not active for this product"));
            }
            return &graph[*dependency];
        }

        Result<std::filesystem::path> dependency_directory(const PackageNode& dependency, const SourceLocation& location) {
            if (!dependency.directory.empty())
                return dependency.directory;

            if (dependency.descriptor) {
                const Value* source = dependency.descriptor->find("source");
                const Value* driver = source ? source->find("driver") : nullptr;
                const Value* path = source ? source->find("path") : nullptr;
                const std::string* driver_name = driver ? driver->as_string() : nullptr;
                const std::string* declared_path = path ? path->as_string() : nullptr;
                if (driver_name && *driver_name == "path" && declared_path) {
                    std::filesystem::path resolved = *declared_path;
                    if (resolved.is_relative() && !path->location().source.empty()) {
                        resolved = std::filesystem::path(path->location().source).parent_path() / resolved;
                    }
                    return std::filesystem::absolute(resolved).lexically_normal();
                }
            }
            return std::unexpected(error_at(location, "dependency `" + dependency.name + "` has no materialized source directory"));
        }

        Result<void> resolve_dependency_paths(const Graph& graph, const PackageNode& package, EffectiveProduct& product) {
            for (const std::string& declared: product.dependency_sources) {
                const std::size_t separator = declared.find(':');
                if (separator == std::string::npos || separator == 0 || separator + 1 == declared.size()) {
                    return std::unexpected(error_at(product.location, "dependency source `" + declared + "` must use `dependency:path`"));
                }
                auto dependency = dependency_by_local_name(
                    graph,
                    package,
                    std::string_view(declared).substr(0, separator),
                    product.location
                );
                if (!dependency)
                    return std::unexpected(dependency.error());

                auto directory = dependency_directory(**dependency, product.location);
                if (!directory)
                    return std::unexpected(directory.error());

                product.dependency_source_files.push_back(
                    (*directory / std::filesystem::path(declared.substr(separator + 1))).lexically_normal()
                );
            }

            for (
                std::vector<std::string>* directories: {&product.include_directories,
                    &product.public_include_directories,
                    &product.system_include_directories,
                    &product.public_system_include_directories}
            ) {
                for (std::string& directory: *directories) {
                    constexpr std::string_view prefix = "${dependency:";
                    if (!directory.starts_with(prefix) || !directory.ends_with('}'))
                        continue;

                    const std::string_view local_name(directory.data() + prefix.size(), directory.size() - prefix.size() - 1);
                    auto dependency = dependency_by_local_name(graph, package, local_name, product.location);
                    if (!dependency)
                        return std::unexpected(dependency.error());

                    auto source_directory = dependency_directory(**dependency, product.location);
                    if (!source_directory)
                        return std::unexpected(source_directory.error());

                    directory = source_directory->generic_string();
                }
            }
            return {};
        }

        Result<EffectiveProduct> realize_product(
            const Graph& graph,
            const ProductDeclaration& declaration,
            const PackageNode& package,
            const ProductRealizationContext& context,
            FileCatalog* files
        ) {
            auto value = effective_product_value(declaration, package, context);
            if (!value)
                return std::unexpected(value.error());

            auto table_result = TableReader::bind(*value, declaration.kind == ProductDeclarationKind::library ? "lib" : "bin");
            if (!table_result)
                return std::unexpected(table_result.error());

            TableReader table = std::move(*table_result);

            EffectiveProduct result;
            result.name = package.name;
            result.location = declaration.location;
            if (declaration.kind == ProductDeclarationKind::executable)
                result.type = EffectiveProductType::executable;
            else
                result.type = EffectiveProductType::static_library;

            auto type = table.optional_string("type");
            if (!type)
                return std::unexpected(type.error());

            if (*type) {
                if (**type == "static")
                    result.type = EffectiveProductType::static_library;
                else if (**type == "shared")
                    result.type = EffectiveProductType::shared_library;
                else if (**type == "interface")
                    result.type = EffectiveProductType::interface_library;
                else
                    return std::unexpected(error_at(table.location_of("type"), "unknown product type `" + **type + "`"));
            }

            auto sources = table.string_array("sources");
            if (!sources)
                return std::unexpected(sources.error());

            result.sources.include = std::move(*sources);
            result.sources.location = declaration.location;

            auto headers = table.string_array("headers");
            if (!headers)
                return std::unexpected(headers.error());

            result.headers.include = std::move(*headers);
            result.headers.location = declaration.location;

            auto public_headers = table.string_array("public-headers");
            if (!public_headers)
                return std::unexpected(public_headers.error());

            result.public_headers.include = std::move(*public_headers);
            result.public_headers.location = declaration.location;

            for (
                const auto& [key, output]: {std::pair{std::string_view{"include"}, &result.include_directories},
                    std::pair{std::string_view{"public-include"}, &result.public_include_directories},
                    std::pair{std::string_view{"system-include"}, &result.system_include_directories},
                    std::pair{std::string_view{"public-system-include"}, &result.public_system_include_directories}}
            ) {
                auto values = table.string_array(key);
                if (!values)
                    return std::unexpected(values.error());

                *output = std::move(*values);
            }

            auto private_definitions = definitions(table, "defines");
            if (!private_definitions)
                return std::unexpected(private_definitions.error());

            result.definitions = std::move(*private_definitions);
            auto public_definitions = definitions(table, "public-defines");
            if (!public_definitions)
                return std::unexpected(public_definitions.error());

            result.public_definitions = std::move(*public_definitions);

            auto system_libraries = table.string_array("system-libraries");
            if (!system_libraries)
                return std::unexpected(system_libraries.error());

            result.system_libraries = std::move(*system_libraries);

            auto dependency_sources = table.string_array("dependency-sources");
            if (!dependency_sources)
                return std::unexpected(dependency_sources.error());

            result.dependency_sources = std::move(*dependency_sources);

            if (const Value* modules = table.take("modules")) {
                const bool* enabled = modules->as_boolean();
                if (!enabled)
                    return std::unexpected(wrong_kind(modules->location(), "a boolean", modules->kind()));

                result.modules = *enabled;
            }

            auto runtime_files = table.string_array("runtime-files");
            if (!runtime_files)
                return std::unexpected(runtime_files.error());

            result.runtime_files.include = std::move(*runtime_files);
            result.runtime_files.location = declaration.location;
            result.policy_layers = package.policy_layers;

            auto finished = table.finish();
            if (!finished)
                return std::unexpected(finished.error());

            auto header_files = expand_file_set(result.headers, package.directory, package.directory, true, files);
            if (!header_files)
                return std::unexpected(header_files.error());

            result.headers.files = std::move(*header_files);
            auto public_header_files = expand_file_set(result.public_headers, package.directory, package.directory, true, files);
            if (!public_header_files)
                return std::unexpected(public_header_files.error());

            result.public_headers.files = std::move(*public_header_files);
            std::erase_if(result.headers.files, [&](const std::filesystem::path& header) {
                return std::ranges::find(result.public_headers.files, header) != result.public_headers.files.end();
            });
            auto runtime_file_paths = expand_file_set(result.runtime_files, package.directory, package.directory, true, files);
            if (!runtime_file_paths)
                return std::unexpected(runtime_file_paths.error());

            result.runtime_files.files = std::move(*runtime_file_paths);
            auto dependency_paths = resolve_dependency_paths(graph, package, result);
            if (!dependency_paths)
                return std::unexpected(dependency_paths.error());

            return result;
        }

        void add_skip_reason(EffectiveTarget& target, std::string reason) {
            target.availability = TargetAvailability::skipped;
            target.skip_reasons.push_back(std::move(reason));
        }

        std::string inactive_feature_reason(const std::string_view package_name, const std::string_view feature) {
            std::string reason;
            reason.reserve(package_name.size() + feature.size() + 15);
            reason.append(package_name);
            reason.push_back('/');
            reason.append(feature);
            reason.append(" is not active");
            return reason;
        }

        EffectiveTarget realize_target(const Graph& graph, const PackageNode& owner, const PackageTarget& target) {
            EffectiveTarget result;
            result.target = target;
            for (const std::string& required: target.required_features) {
                const std::size_t separator = required.find('/');
                const std::string package_name = separator == std::string::npos ? owner.name : required.substr(0, separator);
                const std::string feature = separator == std::string::npos ? required : required.substr(separator + 1);
                const auto package = graph.find_by_name(package_name);
                if (!package || std::ranges::find(graph[*package].active_features, feature) == graph[*package].active_features.end()) {
                    add_skip_reason(result, inactive_feature_reason(package_name, feature));
                }
            }
            for (const auto& [package_name, features]: target.required_dependency_features) {
                const auto package = graph.find_by_name(package_name);
                for (const std::string& feature: features) {
                    if (!package || std::ranges::find(graph[*package].active_features, feature) == graph[*package].active_features.end()) {
                        add_skip_reason(result, inactive_feature_reason(package_name, feature));
                    }
                }
            }
            return result;
        }

        struct SourceClaim {
            bool target = false;
            std::size_t owner = 0;
            std::string pattern;
            bool literal = false;
            SourceLocation location;
        };

        Result<void> add_source_claims(
            std::map<std::filesystem::path, std::vector<SourceClaim>>& claims,
            const FileSet& sources,
            const std::filesystem::path& root,
            const std::filesystem::path& package_directory,
            const bool target,
            const std::size_t owner,
            FileCatalog* files
        ) {
            const bool already_normalized = !sources.files.empty()
                && sources.include.size() == sources.files.size()
                && std::ranges::equal(sources.include, sources.files, {}, std::identity{}, [](const std::filesystem::path& file) {
                       return file.generic_string();
                   });
            if (already_normalized) {
                for (const std::filesystem::path& file: sources.files) {
                    claims[file.lexically_normal()].push_back({target, owner, file.generic_string(), true, sources.location});
                }
                return {};
            }
            for (const std::string& pattern: sources.include) {
                FileSet single{{pattern}, sources.exclude, {}, sources.location};
                auto expanded = expand_file_set(single, root, package_directory, true, files);
                if (!expanded)
                    return std::unexpected(expanded.error());

                for (const std::filesystem::path& file: *expanded) {
                    claims[file.lexically_normal()].push_back({target, owner, pattern, !is_glob_pattern(pattern), sources.location});
                }
            }
            return {};
        }

        Result<void> resolve_source_claims(EffectivePackage& package, const PackageNode& owner, FileCatalog* files) {
            std::map<std::filesystem::path, std::vector<SourceClaim>> claims;
            for (std::size_t index = 0; index < package.products.size(); ++index) {
                auto added = add_source_claims(
                    claims,
                    package.products[index].sources,
                    owner.directory,
                    owner.directory,
                    false,
                    index,
                    files
                );
                if (!added)
                    return std::unexpected(added.error());
            }
            for (std::size_t index = 0; index < package.targets.size(); ++index) {
                const EffectiveTarget& target = package.targets[index];
                if (target.availability == TargetAvailability::skipped)
                    continue;

                auto added = add_source_claims(
                    claims,
                    target.target.sources,
                    target.target.source.parent_path(),
                    owner.directory,
                    true,
                    index,
                    files
                );
                if (!added)
                    return std::unexpected(added.error());
            }

            std::vector<std::set<std::filesystem::path>> product_files(package.products.size());
            std::vector<std::set<std::filesystem::path>> target_files(package.targets.size());
            for (const auto& [file, candidates]: claims) {
                const auto rank = [](const SourceClaim& claim) {
                    if (claim.literal)
                        return 2;

                    return claim.target ? 1 : 0;
                };
                int winning_rank = 0;
                for (const SourceClaim& candidate: candidates)
                    winning_rank = std::max(winning_rank, rank(candidate));

                std::vector<const SourceClaim*> winners;
                for (const SourceClaim& candidate: candidates) {
                    if (rank(candidate) == winning_rank)
                        winners.push_back(&candidate);
                }
                if (winning_rank < 2 && winners.size() > 1) {
                    Diagnostic diagnostic = error_at(
                        winners[1]->location,
                        "source `" + file.generic_string() + "` is claimed by overlapping glob patterns"
                    );
                    for (const SourceClaim* winner: winners) {
                        diagnostic.notes.push_back(
                            std::string(winner->target ? "target" : "product") + " pattern `" + winner->pattern + "`"
                        );
                    }
                    return std::unexpected(std::move(diagnostic));
                }
                for (const SourceClaim* winner: winners) {
                    if (winner->target)
                        target_files[winner->owner].insert(file);
                    else
                        product_files[winner->owner].insert(file);
                }
            }

            for (std::size_t index = 0; index < package.products.size(); ++index) {
                package.products[index].sources.files.assign(product_files[index].begin(), product_files[index].end());
            }
            for (std::size_t index = 0; index < package.targets.size(); ++index) {
                if (package.targets[index].availability == TargetAvailability::available) {
                    package.targets[index].target.sources.files.assign(target_files[index].begin(), target_files[index].end());
                }
            }
            return {};
        }
    }

    std::string host_target_os() {
#if defined(_WIN32)
        return "windows";
#elif defined(__APPLE__)
        return "macos";
#elif defined(__linux__)
        return "linux";
#else
        return "unknown";
#endif
    }

    Result<EffectivePackage> realize_package(
        const Graph& graph,
        const PackageId package_id,
        const ProductRealizationContext& context,
        FileCatalog* files
    ) {
        const PackageNode& package = graph[package_id];
        EffectivePackage result;
        result.package = package_id;
        if (!package.manifest)
            return result;

        for (const ProductDeclaration& declaration: package.manifest->products) {
            auto product = realize_product(graph, declaration, package, context, files);
            if (!product)
                return std::unexpected(product.error());

            result.products.push_back(std::move(*product));
        }
        for (const PackageTarget& target: package.targets) {
            result.targets.push_back(realize_target(graph, package, target));
        }
        auto claims = resolve_source_claims(result, package, files);
        if (!claims)
            return std::unexpected(claims.error());

        return result;
    }
}
