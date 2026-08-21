#pragma once

#include <kaixa/config/table_reader.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace kaixa {
    struct ProviderDefinition {
        std::string name;
        std::string driver;
        bool is_default = false;
        Value options;
        SourceLocation location;
        std::filesystem::path directory;
    };

    [[nodiscard]] Result<std::vector<ProviderDefinition>> read_provider_definitions(TableReader& root);
}
