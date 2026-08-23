#pragma once

#include <kaixa/extension/source.hpp>

#include <functional>
#include <string_view>

namespace kaixa {
    using SourceCachePopulate = std::function<Result<SourceTree>(const std::filesystem::path& destination)>;

    [[nodiscard]] std::filesystem::path default_source_cache();
    [[nodiscard]] Result<SourceTree> materialize_source_cache(
        const SourceContext& context,
        std::string_view driver,
        std::string_view key,
        const SourceCachePopulate& populate
    );
}
