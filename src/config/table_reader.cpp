#include <kaixa/config/table_reader.hpp>
#include <kaixa/config/value_operations.hpp>

#include <utility>

namespace kaixa {
    namespace {
        Diagnostic wrong_kind(SourceLocation location, const std::string_view expected, const ValueKind found) {
            return wrong_value_kind(std::move(location), expected, found);
        }
    }

    TableReader::TableReader(const Value& value, std::string path)
        : m_value(&value)
        , m_path(std::move(path))
        , m_consumed(value.size(), false) {}

    Result<TableReader> TableReader::bind(const Value& value, std::string path) {
        if (!value.is_table()) {
            SourceLocation location = value.location();
            location.config_path = path;
            return std::unexpected(wrong_kind(std::move(location), "a table", value.kind()));
        }
        return TableReader(value, std::move(path));
    }

    const Value* TableReader::take(const std::string_view key) {
        const std::span<const TableEntry> table = entries();
        for (std::size_t index = 0; index < table.size(); ++index) {
            if (table[index].key == key) {
                m_consumed[index] = true;
                return &table[index].value;
            }
        }
        return nullptr;
    }

    void TableReader::take_all() noexcept {
        m_consumed.assign(m_consumed.size(), true);
    }

    std::span<const TableEntry> TableReader::entries() const noexcept {
        const std::vector<TableEntry>* table = m_value->as_table();
        return table ? std::span<const TableEntry>(*table) : std::span<const TableEntry>{};
    }

    SourceLocation TableReader::location_of(const std::string_view key) const {
        const Value* child = m_value->find(key);
        SourceLocation location = child ? child->location() : m_value->location();
        location.config_path = join_config_path(m_path, key);
        return location;
    }

    Result<std::optional<std::string>> TableReader::optional_string(const std::string_view key) {
        const Value* value = take(key);
        if (!value)
            return std::nullopt;

        const std::string* text = value->as_string();
        if (!text)
            return std::unexpected(wrong_kind(location_of(key), "a string", value->kind()));

        return *text;
    }

    Result<std::string> TableReader::string(const std::string_view key) {
        const auto value = optional_string(key);
        if (!value)
            return std::unexpected(value.error());

        if (!*value)
            return std::unexpected(error_at(location_of(key), "missing required key"));

        return **value;
    }

    Result<std::optional<std::vector<std::string>>> TableReader::optional_string_array(const std::string_view key) {
        const Value* value = take(key);
        if (!value)
            return std::nullopt;

        const std::vector<Value>* array = value->as_array();
        if (!array)
            return std::unexpected(wrong_kind(location_of(key), "an array of strings", value->kind()));

        std::vector<std::string> result;
        result.reserve(array->size());
        for (const Value& item: *array) {
            const std::string* text = item.as_string();
            if (!text)
                return std::unexpected(wrong_kind(item.location(), "a string", item.kind()));

            if (text->empty())
                return std::unexpected(error_at(item.location(), "array values cannot be empty"));

            result.push_back(*text);
        }
        return result;
    }

    Result<std::vector<std::string>> TableReader::string_array(const std::string_view key, const bool required) {
        auto value = optional_string_array(key);
        if (!value)
            return std::unexpected(value.error());

        if (*value)
            return std::move(**value);

        if (required)
            return std::unexpected(error_at(location_of(key), "missing required key"));

        return std::vector<std::string>{};
    }

    Result<std::optional<std::int64_t>> TableReader::optional_integer(const std::string_view key) {
        const Value* value = take(key);
        if (!value)
            return std::nullopt;

        const std::int64_t* integer = value->as_integer();
        if (!integer)
            return std::unexpected(wrong_kind(location_of(key), "an integer", value->kind()));

        return *integer;
    }

    Result<std::optional<bool>> TableReader::optional_boolean(const std::string_view key) {
        const Value* value = take(key);
        if (!value)
            return std::nullopt;

        const bool* boolean = value->as_boolean();
        if (!boolean)
            return std::unexpected(wrong_kind(location_of(key), "a boolean", value->kind()));

        return *boolean;
    }

    Result<bool> TableReader::boolean(const std::string_view key, const bool default_value) {
        auto value = optional_boolean(key);
        if (!value)
            return std::unexpected(value.error());

        return value->value_or(default_value);
    }

    Result<TableReader> TableReader::table(const std::string_view key) {
        const Value* value = take(key);
        if (!value)
            return std::unexpected(error_at(location_of(key), "missing required key"));

        return bind(*value, join_config_path(m_path, key));
    }

    Result<std::optional<TableReader>> TableReader::optional_table(const std::string_view key) {
        const Value* value = take(key);
        if (!value)
            return std::nullopt;

        auto reader = bind(*value, join_config_path(m_path, key));
        if (!reader)
            return std::unexpected(reader.error());

        return std::optional<TableReader>(std::move(*reader));
    }

    Result<void> TableReader::finish() const {
        const std::span<const TableEntry> table = entries();
        std::optional<Diagnostic> failure;
        for (std::size_t index = 0; index < table.size(); ++index) {
            if (m_consumed[index])
                continue;

            const std::string path = join_config_path(m_path, table[index].key);
            if (!failure) {
                SourceLocation location = table[index].value.location();
                location.config_path = path;
                failure = error_at(std::move(location), "unknown key `" + path + "`");
            } else {
                failure->notes.push_back("unknown key `" + path + "`");
            }
        }

        if (failure)
            return std::unexpected(std::move(*failure));

        return {};
    }
}
