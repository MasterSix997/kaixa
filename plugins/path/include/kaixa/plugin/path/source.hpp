#pragma once

#include <kaixa/extension/source.hpp>

#include <memory>

namespace kaixa::plugin::path {
    [[nodiscard]] std::unique_ptr<SourceDriver> make_source_driver();
}
