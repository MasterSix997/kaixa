#include <kaixa/foundation/diagnostic.hpp>

#include <string_view>
#include <utility>

namespace kaixa {
    Diagnostic&& Diagnostic::add_note(std::string note) && {
        notes.push_back(std::move(note));
        return std::move(*this);
    }

    Diagnostic error(std::string message) {
        return Diagnostic{std::move(message), std::nullopt, {}, Severity::error};
    }

    Diagnostic error_at(SourceLocation location, std::string message) {
        return Diagnostic{std::move(message), std::move(location), {}, Severity::error};
    }

    Diagnostic warning_at(SourceLocation location, std::string message) {
        return Diagnostic{std::move(message), std::move(location), {}, Severity::warning};
    }

    void DiagnosticSink::report(Diagnostic diagnostic) {
        if (diagnostic.severity == Severity::error)
            ++m_errors;

        m_diagnostics.push_back(std::move(diagnostic));
    }

    const Diagnostic* DiagnosticSink::first_error() const noexcept {
        for (const Diagnostic& diagnostic: m_diagnostics) {
            if (diagnostic.severity == Severity::error)
                return &diagnostic;
        }
        return nullptr;
    }

    namespace {
        std::string_view severity_name(const Severity severity) {
            return severity == Severity::warning ? "warning" : "error";
        }
    }

    std::string format_diagnostic(const Diagnostic& diagnostic) {
        std::string text = std::string(severity_name(diagnostic.severity)) + ": ";

        if (diagnostic.location) {
            const SourceLocation& location = *diagnostic.location;
            bool wrote_location = false;

            if (!location.source.empty()) {
                text += location.source;
                if (location.line != 0) {
                    text += ':';
                    text += std::to_string(location.line);
                    if (location.column != 0) {
                        text += ':';
                        text += std::to_string(location.column);
                    }
                }
                wrote_location = true;
            }

            if (!location.config_path.empty()) {
                if (wrote_location)
                    text += ' ';

                text += '[';
                text += location.config_path;
                text += ']';
                wrote_location = true;
            }

            if (wrote_location)
                text += ": ";
        }

        text += diagnostic.message;
        for (const std::string& note: diagnostic.notes) {
            text += "\n  note: ";
            text += note;
        }
        return text;
    }

    std::string format_diagnostic_short(const Diagnostic& diagnostic) {
        std::string prefix;
        if (diagnostic.location && !diagnostic.location->source.empty()) {
            const SourceLocation& location = *diagnostic.location;
            prefix += location.source;
            prefix += ':';
            prefix += std::to_string(location.line == 0 ? 1 : location.line);
            prefix += ':';
            prefix += std::to_string(location.column == 0 ? 1 : location.column);
            prefix += ": ";
        }

        std::string text = prefix + std::string(severity_name(diagnostic.severity)) + ": " + diagnostic.message;
        if (diagnostic.location
            && !diagnostic.location->config_path.empty()
            && diagnostic.message.find(diagnostic.location->config_path) == std::string::npos) {
            text += " (";
            text += diagnostic.location->config_path;
            text += ')';
        }
        for (const std::string& note: diagnostic.notes) {
            text += '\n';
            text += prefix;
            text += "note: ";
            text += note;
        }
        return text;
    }
}
