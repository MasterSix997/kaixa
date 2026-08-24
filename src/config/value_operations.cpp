#include <kaixa/config/value_operations.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <utility>
#include <vector>

namespace kaixa {
    Diagnostic wrong_value_kind(SourceLocation location, const std::string_view expected, const ValueKind found) {
        return error_at(std::move(location), "expected " + std::string(expected) + ", found " + std::string(value_kind_name(found)));
    }

    Value merge_values(const Value& base, const Value& overlay, const ArrayMerge arrays) {
        const std::vector<TableEntry>* base_table = base.as_table();
        const std::vector<TableEntry>* overlay_table = overlay.as_table();
        if (!base_table || !overlay_table) {
            const std::vector<Value>* base_array = base.as_array();
            const std::vector<Value>* overlay_array = overlay.as_array();
            if (arrays == ArrayMerge::append && base_array && overlay_array) {
                std::vector<Value> merged = *base_array;
                merged.insert(merged.end(), overlay_array->begin(), overlay_array->end());
                return Value::array(std::move(merged), overlay.location());
            }
            return overlay;
        }

        std::vector<TableEntry> merged = *base_table;
        for (const TableEntry& incoming: *overlay_table) {
            const auto existing = std::ranges::find(merged, incoming.key, &TableEntry::key);
            if (existing == merged.end())
                merged.push_back(incoming);
            else
                existing->value = merge_values(existing->value, incoming.value, arrays);
        }
        return Value::table(std::move(merged), overlay.location());
    }

    std::string toml_string(const std::string_view value) {
        constexpr char hexadecimal[] = "0123456789ABCDEF";
        std::string result{"\""};
        for (const char character: value) {
            switch (character) {
            case '\b': result += "\\b"; break;
            case '\t': result += "\\t"; break;
            case '\n': result += "\\n"; break;
            case '\f': result += "\\f"; break;
            case '\r': result += "\\r"; break;
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            default: {
                const auto byte = static_cast<unsigned char>(character);
                if (byte < 0x20U || byte == 0x7FU) {
                    result += "\\u00";
                    result.push_back(hexadecimal[byte >> 4U]);
                    result.push_back(hexadecimal[byte & 0x0FU]);
                } else {
                    result.push_back(character);
                }
            }
            }
        }
        result.push_back('"');
        return result;
    }

    std::string toml_key(const std::string_view key) {
        const bool bare = !key.empty() && std::ranges::all_of(key, [](const char character) {
            return (character >= 'a' && character <= 'z')
                || (character >= 'A' && character <= 'Z')
                || (character >= '0' && character <= '9')
                || character == '_'
                || character == '-';
        });
        return bare ? std::string(key) : toml_string(key);
    }

    Result<std::string> format_inline_toml(const Value& value, const TomlTableOrder order) {
        if (const bool* boolean = value.as_boolean())
            return *boolean ? "true" : "false";

        if (const std::int64_t* integer = value.as_integer())
            return std::to_string(*integer);

        if (const double* floating = value.as_floating()) {
            if (std::isnan(*floating))
                return "nan";

            if (std::isinf(*floating))
                return std::signbit(*floating) ? "-inf" : "inf";

            std::ostringstream output;
            output.imbue(std::locale::classic());
            output << std::setprecision(std::numeric_limits<double>::max_digits10) << *floating;
            std::string result = output.str();
            if (!result.contains('.') && !result.contains('e') && !result.contains('E'))
                result += ".0";

            return result;
        }
        if (const std::string* string = value.as_string())
            return toml_string(*string);

        if (const std::vector<Value>* array = value.as_array()) {
            std::string output{"["};
            for (std::size_t index = 0; index < array->size(); ++index) {
                auto formatted = format_inline_toml((*array)[index], order);
                if (!formatted)
                    return std::unexpected(formatted.error());

                if (index != 0)
                    output += ", ";

                output += *formatted;
            }
            output += ']';
            return output;
        }
        if (const std::vector<TableEntry>* table = value.as_table()) {
            std::vector<const TableEntry*> entries;
            entries.reserve(table->size());
            for (const TableEntry& entry: *table)
                entries.push_back(&entry);

            if (order == TomlTableOrder::sorted)
                std::ranges::sort(entries, {}, [](const TableEntry* entry) { return entry->key; });

            std::string output{"{ "};
            for (std::size_t index = 0; index < entries.size(); ++index) {
                const auto duplicate = std::ranges::find_if(
                    entries.begin(),
                    entries.begin() + static_cast<std::ptrdiff_t>(index),
                    [&](const TableEntry* candidate) { return candidate->key == entries[index]->key; }
                );
                if (duplicate != entries.begin() + static_cast<std::ptrdiff_t>(index))
                    return std::unexpected(error("duplicate value key `" + entries[index]->key + "`"));

                auto formatted = format_inline_toml(entries[index]->value, order);
                if (!formatted)
                    return std::unexpected(formatted.error());

                if (index != 0)
                    output += ", ";

                output += toml_key(entries[index]->key) + " = " + *formatted;
            }
            output += " }";
            return output;
        }
        return std::unexpected(error("cannot write an empty value"));
    }
}
