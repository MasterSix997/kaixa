#include <kaixa/model/manifest.hpp>

#include <kaixa/config/parser.hpp>
#include <kaixa/config/table_reader.hpp>

#include <algorithm>
#include <utility>

namespace kaixa {
    namespace {
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

        Result<DependencySpec> parse_dependency(
            const TableEntry& entry,
            const std::string& path
        ) {
            SourceLocation location = entry.value.location();
            location.config_path = path;

            if (!is_valid_identifier(entry.key))
                return std::unexpected(error_at(
                    location,
                    "`" + entry.key + "` is not a valid dependency name"
                ));

            auto table_result = TableReader::bind(entry.value, path);
            if (!table_result) {
                return std::unexpected(std::move(table_result).error().add_note(
                    "the MVP accepts local dependencies as `{ path = \"...\" }`"
                ));
            }
            TableReader table = std::move(*table_result);

            auto directory = table.string("path");
            if (!directory)
                return std::unexpected(directory.error());
            if (directory->empty())
                return std::unexpected(error_at(table.location_of("path"), "path cannot be empty"));

            auto finished = table.finish();
            if (!finished)
                return std::unexpected(finished.error());

            return DependencySpec{entry.key, std::filesystem::path(*directory), std::move(location)};
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

    Result<Manifest> parse_manifest(const Value& document) {
        auto root_result = TableReader::bind(document);
        if (!root_result)
            return std::unexpected(root_result.error());
        TableReader root = std::move(*root_result);

        auto package_result = root.table("package");
        if (!package_result)
            return std::unexpected(package_result.error());
        TableReader package = std::move(*package_result);

        Manifest manifest;
        auto name = read_identifier(package, "name");
        if (!name)
            return std::unexpected(name.error());
        manifest.name = std::move(*name);
        manifest.location = package.location_of("name");

        auto version = package.optional_string("version");
        if (!version)
            return std::unexpected(version.error());
        if (*version) {
            if ((*version)->empty())
                return std::unexpected(error_at(
                    package.location_of("version"),
                    "version cannot be empty"
                ));
            manifest.version = Version{std::move(**version)};
        }

        auto resolver = read_identifier(package, "resolver");
        if (!resolver)
            return std::unexpected(resolver.error());
        manifest.resolver = std::move(*resolver);

        auto package_finished = package.finish();
        if (!package_finished)
            return std::unexpected(package_finished.error());

        auto dependencies_result = root.optional_table("dependencies");
        if (!dependencies_result)
            return std::unexpected(dependencies_result.error());
        if (*dependencies_result) {
            TableReader dependencies = std::move(**dependencies_result);
            for (const TableEntry& entry: dependencies.entries()) {
                auto dependency = parse_dependency(
                    entry,
                    join_config_path(dependencies.path(), entry.key)
                );
                if (!dependency)
                    return std::unexpected(dependency.error());
                manifest.dependencies.push_back(std::move(*dependency));
            }
            dependencies.take_all();
        }

        auto configurations = read_configuration_set(root);
        if (!configurations)
            return std::unexpected(configurations.error());
        manifest.configurations = std::move(*configurations);

        if (const Value* options = root.take(manifest.resolver)) {
            if (!options->is_table()) {
                return std::unexpected(error_at(
                    options->location(),
                    "resolver options must be a table"
                ));
            }
            manifest.resolver_options = *options;
        }

        auto root_finished = root.finish();
        if (!root_finished)
            return std::unexpected(root_finished.error());
        return manifest;
    }

    Result<Manifest> parse_manifest_file(const std::filesystem::path& path) {
        auto document = parse_file(path);
        if (!document)
            return std::unexpected(document.error());
        auto manifest = parse_manifest(*document);
        if (!manifest)
            return std::unexpected(manifest.error());
        manifest->source = path;
        return manifest;
    }

    Result<Manifest> parse_manifest_string(
        const std::string_view text,
        const std::string_view source_name
    ) {
        auto document = parse_string(text, source_name);
        if (!document)
            return std::unexpected(document.error());
        auto manifest = parse_manifest(*document);
        if (!manifest)
            return std::unexpected(manifest.error());
        manifest->source = std::filesystem::path(source_name);
        return manifest;
    }
}
