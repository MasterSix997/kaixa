#pragma once

#include <kaixa/build/plan.hpp>
#include <kaixa/foundation/diagnostic.hpp>

#include <cstddef>

namespace kaixa {
    struct ExecutionReport {
        std::size_t executed = 0;
    };

    [[nodiscard]] Result<ExecutionReport> execute(const BuildPlan& plan);
}
