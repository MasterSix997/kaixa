#pragma once

#include <kaixa/foundation/diagnostic.hpp>
#include <kaixa/model/manifest.hpp>

#include <filesystem>
#include <optional>
#include <string>

namespace kaixa {
    struct SourceDriverInfo {
        std::string name;
        std::string description;
    };

    struct SourceContext {
        std::filesystem::path requester;
        std::filesystem::path cache;
    };

    struct SourceTree {
        std::filesystem::path directory;
        std::optional<std::string> identity;
    };

    class SourceDriver {
    public:
        virtual ~SourceDriver() = default;

        [[nodiscard]] virtual SourceDriverInfo info() const = 0;
        [[nodiscard]] virtual Result<std::optional<SourceTree>> locate(const SourceLocator& source, const SourceContext& context) const = 0;
    };
}
