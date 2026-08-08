#include <kaixa/config/parser.hpp>

#include <toml/toml.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <utility>

namespace kaixa {
    namespace {
        SourceLocation location_of(
            const toml::node& node,
            const std::string_view source_name,
            std::string config_path
        ) {
            const toml::source_region& region = node.source();
            const std::string source = region.path && !region.path->empty()
                ? *region.path
                : std::string(source_name);
            return {source, region.begin.line, region.begin.column, std::move(config_path)};
        }

        template<typename T>
        std::string spell(const T& value) {
            std::ostringstream text;
            text << value;
            return text.str();
        }

        Value convert(const toml::node& node, std::string_view source_name, std::string path);

        Value convert_table(
            const toml::table& table,
            const std::string_view source_name,
            std::string path
        ) {
            std::vector<TableEntry> entries;
            entries.reserve(table.size());
            for (const auto& [key, child]: table) {
                const std::string_view name = key.str();
                entries.push_back({
                    std::string(name),
                    convert(child, source_name, join_config_path(path, name))
                });
            }

            std::stable_sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
                const SourceLocation& a = left.value.location();
                const SourceLocation& b = right.value.location();
                return a.line == b.line ? a.column < b.column : a.line < b.line;
            });
            return Value::table(
                std::move(entries),
                location_of(table, source_name, std::move(path))
            );
        }

        Value convert_array(
            const toml::array& array,
            const std::string_view source_name,
            std::string path
        ) {
            std::vector<Value> values;
            values.reserve(array.size());
            for (const toml::node& child: array)
                values.push_back(convert(child, source_name, path));
            return Value::array(
                std::move(values),
                location_of(array, source_name, std::move(path))
            );
        }

        Value convert(const toml::node& node, const std::string_view source_name, std::string path) {
            if (const toml::table* table = node.as_table())
                return convert_table(*table, source_name, std::move(path));
            if (const toml::array* array = node.as_array())
                return convert_array(*array, source_name, std::move(path));

            SourceLocation location = location_of(node, source_name, std::move(path));
            if (const auto* value = node.as_string())
                return Value::string(value->get(), std::move(location));
            if (const auto* value = node.as_integer())
                return Value::integer(value->get(), std::move(location));
            if (const auto* value = node.as_floating_point())
                return Value::floating(value->get(), std::move(location));
            if (const auto* value = node.as_boolean())
                return Value::boolean(value->get(), std::move(location));
            if (const auto* value = node.as_date())
                return Value::string(spell(value->get()), std::move(location));
            if (const auto* value = node.as_time())
                return Value::string(spell(value->get()), std::move(location));
            if (const auto* value = node.as_date_time())
                return Value::string(spell(value->get()), std::move(location));
            return {};
        }

        Result<Value> convert_result(toml::parse_result result, const std::string_view source_name) {
            if (!result) {
                const toml::source_region& region = result.error().source();
                const std::string source = region.path && !region.path->empty()
                    ? *region.path
                    : std::string(source_name);
                return std::unexpected(error_at(
                    {source, region.begin.line, region.begin.column, {}},
                    std::string(result.error().description())
                ));
            }
            return convert_table(result.table(), source_name, {});
        }
    }

    Result<Value> parse_string(
        const std::string_view text,
        const std::string_view source_name
    ) {
        return convert_result(toml::parse(text, source_name), source_name);
    }

    Result<Value> parse_file(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return std::unexpected(error_at(
                {path.string(), 0, 0, {}},
                "cannot open file"
            ));
        const std::string source_name = path.string();
        return convert_result(toml::parse(file, source_name), source_name);
    }
}
