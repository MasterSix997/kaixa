#include "target_normalization.hpp"

#include <kaixa/config/parser.hpp>
#include <kaixa/config/value_operations.hpp>
#include <kaixa/model/file_set.hpp>

#include <algorithm>
#include <cctype>
#include <iterator>
#include <utility>

namespace kaixa::workspace_detail {
    namespace {
        std::string target_kind_name(const PackageTargetKind kind) {
            switch (kind) {
            case PackageTargetKind::test: return "test";
            case PackageTargetKind::example: return "example";
            case PackageTargetKind::benchmark: return "benchmark";
            }
            return "target";
        }

        std::string default_target_name(const std::string& package, const PackageTargetKind kind) {
            switch (kind) {
            case PackageTargetKind::test: return package + "_tests";
            case PackageTargetKind::example: return package + "_example";
            case PackageTargetKind::benchmark: return package + "_benchmarks";
            }
            return package + "_target";
        }

        std::string identifier_from_path(std::filesystem::path path) {
            path.replace_extension();
            std::string result = path.generic_string();
            for (char& character: result) {
                const auto byte = static_cast<unsigned char>(character);
                if (!std::isalnum(byte) && character != '_' && character != '-')
                    character = '_';
            }
            return result;
        }

        Value merge_layer_values(const Value& base, const Value& overlay) {
            return merge_values(base, overlay);
        }

        bool path_contains(const std::filesystem::path& directory, const std::filesystem::path& path) {
            const std::filesystem::path relative = path.lexically_relative(directory);
            return !relative.empty() && *relative.begin() != "..";
        }

        void append_unique(std::vector<std::string>& output, const std::vector<std::string>& values) {
            for (const std::string& value: values) {
                if (std::ranges::find(output, value) == output.end())
                    output.push_back(value);
            }
        }

        void merge_entries(std::vector<TableEntry>& output, const std::vector<TableEntry>& values) {
            for (const TableEntry& value: values) {
                const auto existing = std::ranges::find(output, value.key, &TableEntry::key);
                if (existing == output.end())
                    output.push_back(value);
                else
                    *existing = value;
            }
        }

        void merge_target_layer(PackageTarget& target, const PackageTarget& layer) {
            if (layer.display_name)
                target.display_name = layer.display_name;

            if (layer.description)
                target.description = layer.description;

            if (layer.category)
                target.category = layer.category;

            append_unique(target.required_features, layer.required_features);
            for (const auto& [package, features]: layer.required_dependency_features)
                append_unique(target.required_dependency_features[package], features);

            target.dependencies.insert(target.dependencies.end(), layer.dependencies.begin(), layer.dependencies.end());
            if (!layer.arguments.empty())
                target.arguments = layer.arguments;

            target.discover = target.discover || layer.discover;
            target.hidden = target.hidden || layer.hidden;
            target.install = target.install || layer.install;
            if (layer.framework)
                target.framework = layer.framework;

            if (layer.policy) {
                target.policy = target.policy ? std::optional<Value>{merge_layer_values(*target.policy, *layer.policy)} : layer.policy;
            }
            if (layer.resolver_options) {
                target.resolver_options = target.resolver_options
                    ? std::optional<Value>{merge_layer_values(*target.resolver_options, *layer.resolver_options)}
                    : layer.resolver_options;
            }
            if (layer.matrix)
                target.matrix = layer.matrix;

            target.commands.insert(target.commands.end(), layer.commands.begin(), layer.commands.end());
            if (!layer.sources.include.empty()) {
                target.sources = layer.sources;
                target.source = layer.source;
            }
        }

        void replace_capture(std::string& value, const std::string_view capture, const std::string_view replacement) {
            std::size_t position = 0;
            while ((position = value.find(capture, position)) != std::string::npos) {
                value.replace(position, capture.size(), replacement);
                position += replacement.size();
            }
        }

        Result<std::string> matrix_value_text(const Value& value) {
            if (const std::string* text = value.as_string())
                return *text;

            return format_inline_toml(value);
        }

        Value interpolate_value(const Value& value, const std::string_view capture, const std::string_view replacement) {
            if (const std::string* text = value.as_string()) {
                std::string interpolated = *text;
                replace_capture(interpolated, capture, replacement);
                return Value::string(std::move(interpolated), value.location());
            }
            if (const std::vector<Value>* array = value.as_array()) {
                std::vector<Value> interpolated;
                interpolated.reserve(array->size());
                for (const Value& item: *array)
                    interpolated.push_back(interpolate_value(item, capture, replacement));

                return Value::array(std::move(interpolated), value.location());
            }
            if (const std::vector<TableEntry>* table = value.as_table()) {
                std::vector<TableEntry> interpolated;
                interpolated.reserve(table->size());
                for (const TableEntry& entry: *table)
                    interpolated.push_back({entry.key, interpolate_value(entry.value, capture, replacement)});

                return Value::table(std::move(interpolated), value.location());
            }
            return value;
        }

        void interpolate_target(PackageTarget& target, const std::string_view axis, const std::string_view replacement) {
            const std::string capture = "{" + std::string(axis) + "}";
            if (target.name)
                replace_capture(*target.name, capture, replacement);

            if (target.display_name)
                replace_capture(*target.display_name, capture, replacement);

            if (target.description)
                replace_capture(*target.description, capture, replacement);

            if (target.category)
                replace_capture(*target.category, capture, replacement);

            for (std::string& argument: target.arguments)
                replace_capture(argument, capture, replacement);

            for (std::string& include: target.sources.include)
                replace_capture(include, capture, replacement);

            for (std::string& exclude: target.sources.exclude)
                replace_capture(exclude, capture, replacement);

            if (target.policy)
                target.policy = interpolate_value(*target.policy, capture, replacement);

            if (target.resolver_options)
                target.resolver_options = interpolate_value(*target.resolver_options, capture, replacement);
        }

        Result<void> expand_target_matrix(PackageTarget target, const std::size_t axis_index, std::vector<PackageTarget>& output) {
            if (!target.matrix || axis_index == target.matrix->axes.size()) {
                target.matrix.reset();
                output.push_back(std::move(target));
                return {};
            }

            const TargetMatrixAxis axis = target.matrix->axes[axis_index];
            for (const Value& value: axis.values) {
                auto replacement = matrix_value_text(value);
                if (!replacement)
                    return std::unexpected(replacement.error());

                PackageTarget expanded = target;
                interpolate_target(expanded, axis.name, *replacement);
                auto nested = expand_target_matrix(std::move(expanded), axis_index + 1, output);
                if (!nested)
                    return std::unexpected(nested.error());
            }
            return {};
        }

        Result<std::vector<PackageTarget>> expand_target_matrices(std::vector<PackageTarget> declarations) {
            std::vector<PackageTarget> expanded;
            for (PackageTarget& declaration: declarations) {
                auto result = expand_target_matrix(std::move(declaration), 0, expanded);
                if (!result)
                    return std::unexpected(result.error());
            }
            return expanded;
        }

        std::string instantiate_target_name(
            const PackageTarget& target,
            const std::filesystem::path& logical_source,
            const std::string_view logical_name
        ) {
            const std::string stem = logical_name.empty() ? logical_source.stem().string() : std::string(logical_name);
            const std::string parent = logical_source.parent_path().filename().string();
            std::string name = target.name.value_or(stem);
            const bool templated = target.name_template || name.contains("{stem}") || name.contains("{parent}");
            replace_capture(name, "{stem}", stem);
            replace_capture(name, "{parent}", parent);
            if (!templated && name != stem)
                name += "_" + identifier_from_path(logical_source);

            return name;
        }

        Result<bool> target_layer_excludes(
            const PackageTarget& layer,
            const std::filesystem::path& file,
            const std::filesystem::path& package_directory
        ) {
            if (layer.sources.exclude.empty())
                return false;

            FileSet probe;
            probe.include = {file.lexically_relative(layer.source.parent_path()).generic_string()};
            probe.exclude = layer.sources.exclude;
            probe.location = layer.sources.location;
            auto selected = expand_file_set(probe, layer.source.parent_path(), package_directory, true);
            if (!selected)
                return std::unexpected(selected.error());

            return selected->empty();
        }

        Result<std::vector<PackageTarget>> compose_target_directory(
            const std::filesystem::path& root_manifest,
            const PackageTargetKind kind,
            const std::string_view resolver,
            const std::filesystem::path& package_directory,
            FileCatalog* catalog
        ) {
            const std::filesystem::path root_directory = root_manifest.parent_path();
            FileSet documents;
            documents.include = {
                (root_directory / "**" / target_manifest_filename(kind)).lexically_relative(package_directory).generic_string()
            };
            documents.location.source = root_manifest.string();
            auto files = expand_file_set(documents, package_directory, package_directory, false, catalog);
            if (!files)
                return std::unexpected(files.error());

            std::vector<PackageTarget> roots;
            std::vector<PackageTarget> layers;
            for (const std::filesystem::path& relative: *files) {
                const std::filesystem::path absolute = package_directory / relative;
                auto document = parse_file(absolute);
                if (!document)
                    return std::unexpected(document.error());

                auto parsed = parse_package_targets(*document, absolute, kind, resolver);
                if (!parsed)
                    return std::unexpected(parsed.error());

                if (absolute.lexically_normal() == root_manifest.lexically_normal())
                    roots = std::move(*parsed);
                else
                    layers.insert(layers.end(), std::make_move_iterator(parsed->begin()), std::make_move_iterator(parsed->end()));
            }

            std::vector<PackageTarget> result;
            for (const PackageTarget& root: roots) {
                if (!root.each_source) {
                    result.push_back(root);
                    continue;
                }

                auto discovered = expand_file_set(root.sources, root_directory, package_directory, true, catalog);
                if (!discovered)
                    return std::unexpected(discovered.error());

                std::vector<bool> consumed(layers.size(), false);
                for (const std::filesystem::path& relative: *discovered) {
                    PackageTarget concrete = root;
                    const std::filesystem::path absolute = package_directory / relative;
                    const std::filesystem::path local = absolute.lexically_relative(root_directory);
                    bool excluded = false;
                    for (std::size_t index = 0; index < layers.size(); ++index) {
                        const PackageTarget& layer = layers[index];
                        if (layer.kind != kind || !path_contains(layer.source.parent_path(), absolute.parent_path()))
                            continue;

                        if (layer.each_source && layer.partial) {
                            auto layer_excludes = target_layer_excludes(layer, absolute, package_directory);
                            if (!layer_excludes)
                                return std::unexpected(layer_excludes.error());

                            excluded = excluded || *layer_excludes;
                            merge_target_layer(concrete, layer);
                        } else if (!layer.each_source && layer.name && *layer.name == local.stem().string()) {
                            merge_target_layer(concrete, layer);
                            concrete.display_name = layer.display_name;
                            concrete.description = layer.description;
                            consumed[index] = true;
                            excluded = false;
                        }
                    }
                    if (excluded)
                        continue;

                    concrete.each_source = false;
                    concrete.partial = false;
                    concrete.name = instantiate_target_name(root, local, local.stem().string());
                    concrete.source = package_directory / "Kaixa.toml";
                    concrete.sources.include = {relative.generic_string()};
                    concrete.sources.exclude.clear();
                    result.push_back(std::move(concrete));
                }

                for (std::size_t index = 0; index < layers.size(); ++index) {
                    const PackageTarget& declared = layers[index];
                    if (consumed[index] || declared.kind != kind || declared.each_source)
                        continue;

                    PackageTarget concrete = root;
                    const std::filesystem::path group = declared.source.parent_path();
                    for (const PackageTarget& layer: layers) {
                        if (layer.kind == kind && layer.each_source && layer.partial && path_contains(layer.source.parent_path(), group)) {
                            merge_target_layer(concrete, layer);
                        }
                    }
                    merge_target_layer(concrete, declared);
                    const std::string logical_name = declared.name.value_or("target");
                    std::filesystem::path logical_source = group.lexically_relative(root_directory) / (logical_name + ".cpp");
                    concrete.name = instantiate_target_name(root, logical_source, logical_name);
                    concrete.each_source = false;
                    concrete.partial = false;
                    if (declared.sources.include.empty()) {
                        concrete.source = package_directory / "Kaixa.toml";
                        concrete.sources.include = {
                            (group / (logical_name + ".cpp")).lexically_relative(package_directory).generic_string()
                        };
                        concrete.sources.exclude.clear();
                    }
                    result.push_back(std::move(concrete));
                }
            }
            return result;
        }
    }

    Result<std::vector<PackageTarget>> normalize_package_targets(
        const Manifest& manifest,
        const std::filesystem::path& package_directory,
        FileCatalog* catalog
    ) {
        std::vector<PackageTarget> declarations = manifest.targets;
        for (const PackageTargetReference& reference: manifest.target_references) {
            std::filesystem::path declared = reference.path;
            const bool explicit_document = declared.extension() == ".toml";
            const bool directory_reference = !explicit_document && !is_glob_pattern(declared.generic_string());
            if (!explicit_document)
                declared /= target_manifest_filename(reference.kind);

            if (directory_reference) {
                auto composed = compose_target_directory(
                    (package_directory / declared).lexically_normal(),
                    reference.kind,
                    manifest.resolver,
                    package_directory,
                    catalog
                );
                if (!composed)
                    return std::unexpected(composed.error());

                declarations
                    .insert(declarations.end(), std::make_move_iterator(composed->begin()), std::make_move_iterator(composed->end()));
                continue;
            }

            FileSet manifests;
            manifests.include.push_back(declared.generic_string());
            manifests.location = reference.location;
            auto files = expand_file_set(manifests, package_directory, package_directory, false, catalog);
            if (!files)
                return std::unexpected(files.error());

            for (const std::filesystem::path& relative: *files) {
                auto targets = parse_package_targets_file(package_directory / relative, reference.kind, manifest.resolver);
                if (!targets)
                    return std::unexpected(targets.error());

                declarations.insert(declarations.end(), std::make_move_iterator(targets->begin()), std::make_move_iterator(targets->end()));
            }
        }

        auto expanded = expand_target_matrices(std::move(declarations));
        if (!expanded)
            return std::unexpected(expanded.error());

        std::vector<PackageTarget> normalized;
        for (PackageTarget& declared: *expanded) {
            if (declared.kind != PackageTargetKind::test && (declared.discover || !declared.arguments.empty())) {
                return std::unexpected(
                    error_at(declared.location, "discovery and execution arguments are currently supported only for tests")
                );
            }

            const std::filesystem::path source_directory = declared.source.parent_path();
            auto files = expand_file_set(declared.sources, source_directory, package_directory, true, catalog);
            if (!files)
                return std::unexpected(files.error());

            declared.sources.files = std::move(*files);

            if (!declared.each_source) {
                if (!declared.name)
                    declared.name = default_target_name(manifest.name, declared.kind);

                if (!is_valid_target_name(*declared.name)) {
                    return std::unexpected(
                        error_at(declared.location, "target name `" + *declared.name + "` contains an unresolved capture")
                    );
                }

                normalized.push_back(std::move(declared));
                continue;
            }

            const std::string prefix = manifest.name + "_" + target_kind_name(declared.kind);
            const std::filesystem::path relative_source_directory = source_directory.lexically_relative(package_directory);
            for (const std::filesystem::path& file: declared.sources.files) {
                PackageTarget target = declared;
                target.each_source = false;
                target.sources.include = {file.generic_string()};
                target.sources.exclude.clear();
                target.sources.files = {file};

                std::filesystem::path local = file.lexically_relative(relative_source_directory);
                if (local.empty())
                    local = file.filename();

                target.name = declared.name ? instantiate_target_name(declared, local, local.stem().string())
                                            : prefix + "_" + identifier_from_path(local);
                normalized.push_back(std::move(target));
            }
        }

        for (std::size_t index = 0; index < normalized.size(); ++index) {
            const auto duplicate = std::ranges::find_if(
                normalized.begin(),
                normalized.begin() + static_cast<std::ptrdiff_t>(index),
                [&](const PackageTarget& candidate) { return candidate.name == normalized[index].name; }
            );
            if (duplicate != normalized.begin() + static_cast<std::ptrdiff_t>(index)) {
                return std::unexpected(error_at(normalized[index].location, "duplicate package target `" + *normalized[index].name + "`"));
            }
        }
        return normalized;
    }
}
