#pragma once

#include <kaixa/foundation/diagnostic.hpp>
#include <kaixa/model/manifest.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace kaixa {
    struct SourceDriverInfo {
        std::string name;
        std::string description;
    };

    struct SourceContext {
        std::filesystem::path requester;
        std::filesystem::path cache;
        bool offline = false;
        bool refresh = false;
        std::optional<std::string> expected_identity;
        std::optional<std::string> expected_integrity;
        std::function<void(std::string_view)> progress;
    };

    struct SourceTree {
        std::filesystem::path directory;
        std::optional<std::string> identity;
        std::optional<std::string> integrity;
        bool cache_hit = false;
    };

    class SourceDriver {
    public:
        virtual ~SourceDriver() = default;

        [[nodiscard]] virtual SourceDriverInfo info() const = 0;
        [[nodiscard]] virtual Result<std::optional<SourceTree>> locate(const SourceLocator& source, const SourceContext& context) const = 0;

        [[nodiscard]] virtual Result<std::optional<SourceTree>> materialize(
            const SourceLocator& source,
            const SourceContext& context
        ) const {
            return locate(source, context);
        }
    };
}
