#pragma once

#include <kaixa/config/value.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace kaixa::plugin::cmake::detail::syntax {
    class Writer {
    public:
        void append(std::string_view text);
        void line(std::string_view text = {});
        void indent() noexcept { ++m_indent; }
        void outdent() noexcept;

        [[nodiscard]] std::string finish() && { return std::move(m_output); }

    private:
        std::string m_output;
        std::size_t m_indent = 0;
    };

    [[nodiscard]] std::string literal(std::string_view value);
    [[nodiscard]] std::string expanding_literal(std::string_view value);
    [[nodiscard]] std::string argument(std::string_view value);
    [[nodiscard]] std::string path(const std::filesystem::path& value);
    [[nodiscard]] Result<std::string> scalar(const Value& value);
}
