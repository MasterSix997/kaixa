#pragma once

#include <kaixa/foundation/diagnostic.hpp>

#include <filesystem>
#include <string>
#include <string_view>

namespace kaixa {
    [[nodiscard]] Result<std::string> read_file(const std::filesystem::path& path);
    [[nodiscard]] Result<void> write_file(
        const std::filesystem::path& path,
        std::string_view contents
    );
}
