#pragma once

#include <kaixa/config/value.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kaixa {
    class TableReader {
    public:
        [[nodiscard]] static Result<TableReader> bind(const Value& value, std::string path = {});

        const Value* take(std::string_view key);
        void take_all() noexcept;

        [[nodiscard]] std::span<const TableEntry> entries() const noexcept;
        [[nodiscard]] Result<std::string> string(std::string_view key);
        [[nodiscard]] Result<std::optional<std::string>> optional_string(std::string_view key);
        [[nodiscard]] Result<TableReader> table(std::string_view key);
        [[nodiscard]] Result<std::optional<TableReader>> optional_table(std::string_view key);
        [[nodiscard]] Result<void> finish() const;

        [[nodiscard]] const std::string& path() const noexcept { return m_path; }
        [[nodiscard]] SourceLocation location_of(std::string_view key) const;

    private:
        TableReader(const Value& value, std::string path);

        const Value* m_value;
        std::string m_path;
        std::vector<bool> m_consumed;
    };
}
