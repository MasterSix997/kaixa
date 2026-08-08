#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace kaixa {
    struct SourceLocation {
        std::string source;
        std::size_t line = 0;
        std::size_t column = 0;
        std::string config_path;
    };

    struct Diagnostic {
        std::string message;
        std::optional<SourceLocation> location;
        std::vector<std::string> notes;

        [[nodiscard]] Diagnostic&& add_note(std::string note) &&;
    };

    template<typename T>
    using Result = std::expected<T, Diagnostic>;

    [[nodiscard]] Diagnostic error(std::string message);
    [[nodiscard]] Diagnostic error_at(SourceLocation location, std::string message);
    [[nodiscard]] std::string format_diagnostic(const Diagnostic& diagnostic);
}
