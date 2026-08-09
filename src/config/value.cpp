#include <kaixa/config/value.hpp>

#include <utility>

namespace kaixa {
    std::string_view value_kind_name(const ValueKind kind) noexcept {
        switch (kind) {
            case ValueKind::none: return "nothing";
            case ValueKind::boolean: return "boolean";
            case ValueKind::integer: return "integer";
            case ValueKind::floating: return "float";
            case ValueKind::string: return "string";
            case ValueKind::array: return "array";
            case ValueKind::table: return "table";
        }
        return "nothing";
    }

    std::string join_config_path(const std::string_view prefix, const std::string_view key) {
        if (prefix.empty())
            return std::string(key);
        return std::string(prefix) + '.' + std::string(key);
    }

    Value::Value() noexcept = default;
    Value::Value(const bool value) : Value(Storage{value}, {}) {
    }

    Value::Value(const std::int64_t value) : Value(Storage{value}, {}) {
    }

    Value::Value(const double value) : Value(Storage{value}, {}) {
    }

    Value::Value(const char* value) : Value(std::string(value)) {
    }

    Value::Value(std::string value) : Value(Storage{std::move(value)}, {}) {
    }

    Value::Value(const std::string_view value) : Value(std::string(value)) {
    }

    Value::~Value() = default;
    Value::Value(const Value& other) = default;
    Value::Value(Value&& other) noexcept = default;
    Value& Value::operator=(const Value& other) = default;
    Value& Value::operator=(Value&& other) noexcept = default;

    Value::Value(Storage storage, SourceLocation location)
        : m_storage(std::move(storage)), m_location(std::move(location)) {
    }

    Value Value::boolean(const bool value, SourceLocation location) {
        return Value(Storage{value}, std::move(location));
    }

    Value Value::integer(const std::int64_t value, SourceLocation location) {
        return Value(Storage{value}, std::move(location));
    }

    Value Value::floating(const double value, SourceLocation location) {
        return Value(Storage{value}, std::move(location));
    }

    Value Value::string(std::string value, SourceLocation location) {
        return Value(Storage{std::move(value)}, std::move(location));
    }

    Value Value::array(std::vector<Value> values, SourceLocation location) {
        return Value(Storage{std::move(values)}, std::move(location));
    }

    Value Value::table(std::vector<TableEntry> entries, SourceLocation location) {
        return Value(Storage{std::move(entries)}, std::move(location));
    }

    ValueKind Value::kind() const noexcept {
        return static_cast<ValueKind>(m_storage.index());
    }

    const bool* Value::as_boolean() const noexcept { return std::get_if<bool>(&m_storage); }
    const std::int64_t* Value::as_integer() const noexcept {
        return std::get_if<std::int64_t>(&m_storage);
    }
    const double* Value::as_floating() const noexcept {
        return std::get_if<double>(&m_storage);
    }
    const std::string* Value::as_string() const noexcept {
        return std::get_if<std::string>(&m_storage);
    }
    const std::vector<Value>* Value::as_array() const noexcept {
        return std::get_if<std::vector<Value>>(&m_storage);
    }
    const std::vector<TableEntry>* Value::as_table() const noexcept {
        return std::get_if<std::vector<TableEntry>>(&m_storage);
    }

    const Value* Value::find(const std::string_view key) const noexcept {
        const std::vector<TableEntry>* table = as_table();
        if (!table)
            return nullptr;
        for (const TableEntry& entry: *table) {
            if (entry.key == key)
                return &entry.value;
        }
        return nullptr;
    }

    std::size_t Value::size() const noexcept {
        if (const std::vector<Value>* array = as_array())
            return array->size();
        if (const std::vector<TableEntry>* table = as_table())
            return table->size();
        return 0;
    }
}
