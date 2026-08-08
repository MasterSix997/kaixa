#include <kaixa/foundation/diagnostic.hpp>

#include <utility>

namespace kaixa {
    Diagnostic&& Diagnostic::add_note(std::string note) && {
        notes.push_back(std::move(note));
        return std::move(*this);
    }

    Diagnostic error(std::string message) {
        return Diagnostic{std::move(message), std::nullopt, {}};
    }

    Diagnostic error_at(SourceLocation location, std::string message) {
        return Diagnostic{std::move(message), std::move(location), {}};
    }

    std::string format_diagnostic(const Diagnostic& diagnostic) {
        std::string text = "error: ";

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
}
