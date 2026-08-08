#pragma once

#include <string>

namespace kaixa {
    struct Version {
        std::string text;

        [[nodiscard]] bool operator==(const Version&) const = default;
    };
}
