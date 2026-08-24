#pragma once

#include <kaixa/config/value.hpp>
#include <kaixa/foundation/diagnostic.hpp>

#include <string>
#include <string_view>

namespace kaixa {
    enum class TomlTableOrder {
        declared,
        sorted
    };

    enum class ArrayMerge {
        replace,
        append
    };

    [[nodiscard]] Diagnostic wrong_value_kind(SourceLocation location, std::string_view expected, ValueKind found);
    [[nodiscard]] Value merge_values(const Value& base, const Value& overlay, ArrayMerge arrays = ArrayMerge::replace);
    [[nodiscard]] std::string toml_string(std::string_view value);
    [[nodiscard]] std::string toml_key(std::string_view key);
    [[nodiscard]] Result<std::string> format_inline_toml(const Value& value, TomlTableOrder order = TomlTableOrder::declared);
}
