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

    // Names a phase when reporting on a plan. Actions never carry it: a plan's phases are its
    // containers, so this only labels results.
    enum class ExecutionPhase {
        synchronize,
        build,
        task,
        test
    };

    struct GeneratedFileCheck {
        std::filesystem::path path;
        GeneratedFileState state = GeneratedFileState::current;
    };

    struct ActionCheck {
        std::string description;
        ActionState state = ActionState::unknown;
    };

    struct CheckReport {
        std::vector<GeneratedFileCheck> generated_files;
        std::vector<ActionCheck> synchronization;
        std::vector<ActionCheck> build;
        std::vector<ActionCheck> tasks;
        std::vector<ActionCheck> tests;

        [[nodiscard]] bool requires_synchronization() const noexcept;
        [[nodiscard]] std::span<const ActionCheck> phase(ExecutionPhase phase) const noexcept;
    };

    struct GenerationReport {
        std::size_t written = 0;
        std::size_t unchanged = 0;
        std::size_t synchronized = 0;
    };

    struct ExecutionReport {
        std::size_t executed = 0;
    };

    [[nodiscard]] Result<CheckReport> check(const ExecutionPlan& plan);
    [[nodiscard]] Result<GenerationReport> generate(const ExecutionPlan& plan);
    [[nodiscard]] Result<ExecutionReport> execute_actions(const ExecutionPlan& plan, ExecutionPhase phase);
    [[nodiscard]] Result<ExecutionReport> execute(const ExecutionPlan& plan);
    [[nodiscard]] Result<ExecutionReport> test(const ExecutionPlan& plan);
}
