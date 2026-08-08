#pragma once

#include <kaixa/extension/resolver.hpp>

#include <memory>

namespace kaixa::plugin::cmake {
    [[nodiscard]] std::unique_ptr<Resolver> make_resolver();
}
