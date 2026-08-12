#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace kaixa {
    enum class ActionState {
        current,
        required,
        unknown
    };

    enum class ActionStage {
        synchronize,
        build,
        test
    };

    struct Action {
        Action() = default;

        Action(
            std::string description,
            std::vector<std::string> argv,
            std::filesystem::path working_directory,
            std::vector<std::filesystem::path> inputs,
            std::vector<std::filesystem::path> outputs,
            std::optional<ActionState> checked_state = std::nullopt,
            const ActionStage stage = ActionStage::build
        ) : description(std::move(description)),
            argv(std::move(argv)),
            working_directory(std::move(working_directory)),
            inputs(std::move(inputs)),
            outputs(std::move(outputs)),
            checked_state(checked_state),
            stage(stage) {
        }

        std::string description;
        std::vector<std::string> argv;
        std::filesystem::path working_directory;
        std::vector<std::filesystem::path> inputs;
        std::vector<std::filesystem::path> outputs;
        std::optional<ActionState> checked_state;
        ActionStage stage = ActionStage::build;
    };
}
