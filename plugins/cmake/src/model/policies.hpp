#pragma once

#include <kaixa/model/policy.hpp>

#include <span>

namespace kaixa::plugin::cmake {
    [[nodiscard]] std::span<const PolicyDefinition> native_policy_definitions();
}
