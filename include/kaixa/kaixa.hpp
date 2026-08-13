#pragma once

#include <kaixa/build/executor.hpp>
#include <kaixa/build/product.hpp>
#include <kaixa/clean/plan.hpp>
#include <kaixa/config/parser.hpp>
#include <kaixa/extension/registry.hpp>
#include <kaixa/foundation/diagnostic.hpp>
#include <kaixa/model/graph.hpp>
#include <kaixa/model/manifest.hpp>
#include <kaixa/services/build_service.hpp>
#include <kaixa/services/clean_service.hpp>
#include <kaixa/services/run_service.hpp>
#include <kaixa/workspace/loader.hpp>

#include <string_view>

namespace kaixa {
    [[nodiscard]] std::string_view version() noexcept;
}
