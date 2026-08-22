#pragma once

#include <kaixa/foundation/process.hpp>
#include <kaixa/model/package.hpp>

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
        task,
        test
    };

    struct Action {
        Action() = default;

        Action(
            std::string action_description,
            std::vector<std::string> action_argv,
            std::filesystem::path action_working_directory,
            std::vector<std::filesystem::path> action_inputs,
            std::vector<std::filesystem::path> action_outputs,
            std::optional<ActionState> known_state = std::nullopt,
            const ActionStage action_stage = ActionStage::build
        )
            : description(std::move(action_description))
            , argv(std::move(action_argv))
            , working_directory(std::move(action_working_directory))
            , inputs(std::move(action_inputs))
            , outputs(std::move(action_outputs))
            , checked_state(known_state)
            , stage(action_stage) {}

        std::string description;
        std::vector<std::string> argv;
        std::filesystem::path working_directory;
        std::vector<std::filesystem::path> inputs;
        std::vector<std::filesystem::path> outputs;
        std::vector<EnvironmentVariable> environment;
        std::optional<ActionState> checked_state;
        std::optional<PackageId> package;
        std::optional<std::string> configured_artifact;
        ActionStage stage = ActionStage::build;
    };
}
