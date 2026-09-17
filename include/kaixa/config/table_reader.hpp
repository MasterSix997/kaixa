#pragma once

#include <kaixa/config/value.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kaixa {
    class TableReader {
    public:
        [[nodiscard]] static Result<TableReader> bind(const Value& value, std::string path = {}, DiagnosticSink* sink = nullptr);

        const Value* take(std::string_view key);
        void take_all() noexcept;
        // Hands every still unconsumed entry to the table's owner as one opaque value.
        [[nodiscard]] Value take_remaining();

        [[nodiscard]] std::span<const TableEntry> entries() const noexcept;
        [[nodiscard]] Result<std::string> string(std::string_view key);
        [[nodiscard]] Result<std::optional<std::string>> optional_string(std::string_view key);
        [[nodiscard]] Result<std::optional<std::vector<std::string>>> optional_string_array(std::string_view key);
        [[nodiscard]] Result<std::vector<std::string>> string_array(std::string_view key, bool required = false);
        [[nodiscard]] Result<std::optional<std::int64_t>> optional_integer(std::string_view key);
        [[nodiscard]] Result<std::optional<bool>> optional_boolean(std::string_view key);
        [[nodiscard]] Result<bool> boolean(std::string_view key, bool default_value = false);
        [[nodiscard]] Result<TableReader> table(std::string_view key);
        [[nodiscard]] Result<std::optional<TableReader>> optional_table(std::string_view key);
        [[nodiscard]] Result<void> finish() const;

        [[nodiscard]] const std::string& path() const noexcept { return m_path; }
        [[nodiscard]] DiagnosticSink* sink() const noexcept { return m_sink; }
        [[nodiscard]] SourceLocation location_of(std::string_view key) const;

    private:
        TableReader(const Value& value, std::string path, DiagnosticSink* sink);

        const Value* m_value;
        std::string m_path;
        std::vector<bool> m_consumed;
        DiagnosticSink* m_sink = nullptr;
    };
}
