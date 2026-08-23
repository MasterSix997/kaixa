#pragma once

#include <kaixa/foundation/diagnostic.hpp>

#include <filesystem>
#include <string>
#include <string_view>

namespace kaixa {
    [[nodiscard]] std::string sha256(std::string_view contents);
    [[nodiscard]] Result<std::string> sha256_file(const std::filesystem::path& path);
}
