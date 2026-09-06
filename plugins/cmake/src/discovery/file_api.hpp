#pragma once

#include <kaixa/build/action.hpp>
#include <kaixa/foundation/diagnostic.hpp>

#include <filesystem>
#include <span>

namespace kaixa::plugin::cmake::detail {
    [[nodiscard]] std::filesystem::path file_api_query(const std::filesystem::path& build);

    [[nodiscard]] Result<ActionState> configuration_state(
        const std::filesystem::path& build,
        std::span<const std::filesystem::path> explicit_inputs
    );
}
