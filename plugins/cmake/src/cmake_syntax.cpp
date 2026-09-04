#include "cmake_syntax.hpp"

#include <array>
#include <charconv>
#include <system_error>

namespace kaixa::plugin::cmake::detail::syntax {
    void Writer::append(const std::string_view text) {
        m_output.append(text);
    }

    void Writer::line(const std::string_view text) {
        m_output.append(m_indent * 2, ' ');
        m_output.append(text);
        m_output.push_back('\n');
    }

    void Writer::outdent() noexcept {
        if (m_indent != 0)
            --m_indent;
    }

    std::string literal(const std::string_view value) {
        std::string equals;
        while (value.contains("]" + equals + "]"))
            equals += '=';

        return "[" + equals + "[" + std::string(value) + "]" + equals + "]";
    }

    std::string expanding_literal(const std::string_view value) {
        std::string result = "\"";
        for (const char character: value) {
            if (character == '\\' || character == '"')
                result.push_back('\\');

            result.push_back(character);
        }
        result.push_back('"');
        return result;
    }

    std::string argument(const std::string_view value) {
        return value.contains("${") ? expanding_literal(value) : literal(value);
    }

    std::string path(const std::filesystem::path& value) {
        return literal(value.generic_string());
    }

    Result<std::string> scalar(const Value& value) {
        if (const bool* boolean = value.as_boolean())
            return *boolean ? "ON" : "OFF";

        if (const std::int64_t* integer = value.as_integer())
            return std::to_string(*integer);

        if (const double* floating = value.as_floating()) {
            std::array<char, 64> buffer{};
            const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), *floating);
            if (converted.ec != std::errc{})
                return std::unexpected(error_at(value.location(), "cannot represent CMake floating-point value"));

            return std::string(buffer.data(), converted.ptr);
        }
        if (const std::string* string = value.as_string())
            return literal(*string);

        return std::unexpected(error_at(value.location(), "CMake values must be scalar"));
    }
}
