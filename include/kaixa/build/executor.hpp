#pragma once

#include <kaixa/build/plan.hpp>
#include <kaixa/foundation/diagnostic.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace kaixa {
    enum class GeneratedFileState {
        current,
        missing,
        different
    };

    struct GeneratedFileCheck {
        std::filesystem::path path;
        GeneratedFileState state = GeneratedFileState::current;
    };

    struct ActionCheck {
        std::string description;
        ActionState state = ActionState::unknown;
        ActionStage stage = ActionStage::build;
    };

    struct CheckReport {
        std::vector<GeneratedFileCheck> generated_files;
        std::vector<ActionCheck> actions;

        [[nodiscard]] bool requires_synchronization() const noexcept;
    };

    struct GenerationReport {
        std::size_t written = 0;
        std::size_t unchanged = 0;
        std::size_t synchronized = 0;
    };

    struct ExecutionReport {
        std::size_t executed = 0;
    };

    [[nodiscard]] Result<CheckReport> check(const BuildPlan& plan);
    [[nodiscard]] Result<GenerationReport> generate(const BuildPlan& plan);
    [[nodiscard]] Result<ExecutionReport> execute_actions(
        const BuildPlan& plan,
        ActionStage stage
    );
    [[nodiscard]] Result<ExecutionReport> execute(const BuildPlan& plan);
}
