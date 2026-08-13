#pragma once

#include <kaixa/config/value.hpp>

#include <filesystem>
#include <string_view>

namespace kaixa {
    [[nodiscard]] Result<Value> parse_file(const std::filesystem::path& path);
    [[nodiscard]] Result<Value> parse_string(std::string_view text, std::string_view source_name);
}
