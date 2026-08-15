#pragma once

#include <kaixa/config/table_reader.hpp>

#include <string>
#include <vector>

namespace kaixa {
    struct ProviderDefinition {
        std::string name;
        std::string driver;
        bool is_default = false;
        Value options;
        SourceLocation location;
    };

    [[nodiscard]] Result<std::vector<ProviderDefinition>> read_provider_definitions(TableReader& root);
}
