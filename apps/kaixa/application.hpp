#pragma once

#include "command_line.hpp"

namespace kaixa::cli {
    [[nodiscard]] int execute(const Command& command);
}
