#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
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

    enum class Severity {
        error,
        warning
    };

    struct Diagnostic {
        std::string message;
        std::optional<SourceLocation> location;
        std::vector<std::string> notes;
        Severity severity = Severity::error;

        [[nodiscard]] Diagnostic&& add_note(std::string note) &&;
    };

    template <typename T> using Result = std::expected<T, Diagnostic>;

    class DiagnosticSink {
    public:
        void report(Diagnostic diagnostic);

        [[nodiscard]] std::span<const Diagnostic> diagnostics() const noexcept { return m_diagnostics; }
        [[nodiscard]] bool has_errors() const noexcept { return m_errors != 0; }
        [[nodiscard]] std::size_t errors() const noexcept { return m_errors; }
        [[nodiscard]] const Diagnostic* first_error() const noexcept;

    private:
        std::vector<Diagnostic> m_diagnostics;
        std::size_t m_errors = 0;
    };

    [[nodiscard]] Diagnostic error(std::string message);
    [[nodiscard]] Diagnostic error_at(SourceLocation location, std::string message);
    [[nodiscard]] Diagnostic warning_at(SourceLocation location, std::string message);
    [[nodiscard]] std::string format_diagnostic(const Diagnostic& diagnostic);
    [[nodiscard]] std::string format_diagnostic_short(const Diagnostic& diagnostic);
}
