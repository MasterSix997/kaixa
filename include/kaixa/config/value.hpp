#pragma once

#include <kaixa/foundation/diagnostic.hpp>

#include <cstddef>
#include <cstdint>
#include <concepts>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace kaixa {
    enum class ValueKind {
        none,
        boolean,
        integer,
        floating,
        string,
        array,
        table
    };

    [[nodiscard]] std::string_view value_kind_name(ValueKind kind) noexcept;
    [[nodiscard]] std::string join_config_path(std::string_view prefix, std::string_view key);

    struct TableEntry;

    class Value {
    public:
        Value() noexcept;
        Value(bool value);
        Value(std::int64_t value);
        Value(double value);
        Value(const char* value);
        Value(std::string value);
        Value(std::string_view value);

        template<typename Integer>
            requires std::integral<Integer>
                && (!std::same_as<std::remove_cv_t<Integer>, bool>)
                && (!std::same_as<std::remove_cv_t<Integer>, std::int64_t>)
        Value(const Integer value) : Value(static_cast<std::int64_t>(value)) {
        }

        ~Value();
        Value(const Value& other);
        Value(Value&& other) noexcept;
        Value& operator=(const Value& other);
        Value& operator=(Value&& other) noexcept;

        [[nodiscard]] static Value boolean(bool value, SourceLocation location = {});
        [[nodiscard]] static Value integer(std::int64_t value, SourceLocation location = {});
        [[nodiscard]] static Value floating(double value, SourceLocation location = {});
        [[nodiscard]] static Value string(std::string value, SourceLocation location = {});
        [[nodiscard]] static Value array(std::vector<Value> values, SourceLocation location = {});
        [[nodiscard]] static Value table(
            std::vector<TableEntry> entries,
            SourceLocation location = {}
        );

        [[nodiscard]] ValueKind kind() const noexcept;
        [[nodiscard]] const SourceLocation& location() const noexcept { return m_location; }
        [[nodiscard]] bool is_table() const noexcept { return kind() == ValueKind::table; }

        [[nodiscard]] const bool* as_boolean() const noexcept;
        [[nodiscard]] const std::int64_t* as_integer() const noexcept;
        [[nodiscard]] const double* as_floating() const noexcept;
        [[nodiscard]] const std::string* as_string() const noexcept;
        [[nodiscard]] const std::vector<Value>* as_array() const noexcept;
        [[nodiscard]] const std::vector<TableEntry>* as_table() const noexcept;
        [[nodiscard]] const Value* find(std::string_view key) const noexcept;
        [[nodiscard]] std::size_t size() const noexcept;

    private:
        using Storage = std::variant<
            std::monostate,
            bool,
            std::int64_t,
            double,
            std::string,
            std::vector<Value>,
            std::vector<TableEntry>
        >;

        Value(Storage storage, SourceLocation location);

        Storage m_storage;
        SourceLocation m_location;
    };

    struct TableEntry {
        std::string key;
        Value value;
    };
}
