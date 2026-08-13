#pragma once

#include <kaixa/build/layout.hpp>
#include <kaixa/clean/plan.hpp>
#include <kaixa/extension/registry.hpp>
#include <kaixa/foundation/diagnostic.hpp>
#include <kaixa/model/graph.hpp>

#include <cstdint>

namespace kaixa {
    struct CleanReport {
        std::size_t removed_paths = 0;
        std::uintmax_t removed_entries = 0;
    };

    [[nodiscard]] Result<CleanPlan> plan_clean(
        const Graph& graph,
        const ResolverRegistry& registry,
        const BuildEnvironment& environment,
        const CleanRequest& request = {}
    );

    [[nodiscard]] Result<CleanReport> clean(
        const CleanPlan& plan,
        const std::filesystem::path& state_root,
        const std::filesystem::path& workspace,
        bool dry_run = false,
        bool allow_state_root = false
    );
}
