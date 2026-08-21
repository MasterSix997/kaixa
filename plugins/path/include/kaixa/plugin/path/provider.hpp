#pragma once

#include <kaixa/extension/provider.hpp>

#include <memory>

namespace kaixa::plugin::path {
    [[nodiscard]] std::unique_ptr<ProviderDriver> make_provider_driver();
    [[nodiscard]] std::unique_ptr<ProviderDriver> make_package_map_provider_driver();
    [[nodiscard]] std::unique_ptr<ProviderDriver> make_system_packages_provider_driver();
    [[nodiscard]] std::unique_ptr<ProviderDriver> make_registry_provider_driver();
}
