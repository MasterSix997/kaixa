#pragma once

#include <kaixa/extension/provider.hpp>

#include <memory>

namespace kaixa::plugin::registry {
    [[nodiscard]] std::unique_ptr<ProviderDriver> make_provider_driver();
}
