#include <kaixa/build/executor.hpp>

#include <kaixa/foundation/filesystem.hpp>
#include <kaixa/foundation/process.hpp>

#include <algorithm>
#include <optional>
#include <system_error>

namespace kaixa {
    namespace {
        bool same_path(const std::filesystem::path& left, const std::filesystem::path& right) {
            return left.lexically_normal() == right.lexically_normal();
        }

        bool consumes_changed_path(const Action& action, const std::vector<std::filesystem::path>& changed) {
            return std::ranges::any_of(action.inputs, [&](const std::filesystem::path& input) {
                return std::ranges::any_of(changed, [&](const std::filesystem::path& path) {
                    return same_path(input, path);
                });
            });
        }

        void append_changed_outputs(std::vector<std::filesystem::path>& changed, const Action& action) {
            for (const std::filesystem::path& output: action.outputs) {
                if (std::ranges::none_of(changed, [&](const std::filesystem::path& path) {
                        return same_path(output, path);
                    })) {
                    changed.push_back(output);
                }
            }
        }

        Result<GeneratedFileState> generated_file_state(const GeneratedFile& generated) {
            std::error_code failure;
            const bool exists = std::filesystem::exists(generated.path, failure);
            if (failure) {
                return std::unexpected(error(
                    "cannot inspect generated file `" + generated.path.string()
                        + "`: " + failure.message()
                ));
            }
            if (!exists)
                return GeneratedFileState::missing;
            if (!std::filesystem::is_regular_file(generated.path, failure) || failure)
                return GeneratedFileState::different;

            auto content = read_file(generated.path);
            if (!content)
                return std::unexpected(content.error());

            return *content == generated.content
                ? GeneratedFileState::current
                : GeneratedFileState::different;
        }

        Result<ActionState> action_state(const Action& action) {
            if (action.checked_state)
                return *action.checked_state;
            if (action.outputs.empty())
                return ActionState::unknown;

            std::optional<std::filesystem::file_time_type> oldest_output;
            for (const std::filesystem::path& output: action.outputs) {
                std::error_code failure;
                if (!std::filesystem::exists(output, failure)) {
                    if (failure) {
                        return std::unexpected(error(
                            "cannot inspect action output `" + output.string() + "`: " + failure.message()
                        ));
                    }
                    return ActionState::required;
                }
                if (!std::filesystem::is_regular_file(output, failure) || failure)
                    return ActionState::unknown;

                const auto modified = std::filesystem::last_write_time(output, failure);
                if (failure) {
                    return std::unexpected(error(
                        "cannot inspect action output `" + output.string() + "`: " + failure.message()
                    ));
                }
                if (!oldest_output || modified < *oldest_output)
                    oldest_output = modified;
            }

            for (const std::filesystem::path& input: action.inputs) {
                std::error_code failure;
                if (!std::filesystem::exists(input, failure)) {
                    if (failure) {
                        return std::unexpected(error(
                            "cannot inspect action input `" + input.string() + "`: " + failure.message()
                        ));
                    }
                    return ActionState::required;
                }
                if (!std::filesystem::is_regular_file(input, failure) || failure)
                    return ActionState::unknown;

                const auto modified = std::filesystem::last_write_time(input, failure);
                if (failure) {
                    return std::unexpected(error(
                        "cannot inspect action input `" + input.string() + "`: " + failure.message()
                    ));
                }
                if (oldest_output && modified > *oldest_output)
                    return ActionState::unknown;
            }
            return ActionState::current;
        }

        Result<void> execute_action(const Action& action) {
            const ProcessRequest request{action.argv, action.working_directory};
            auto result = run_process(request);
            if (!result) {
                return std::unexpected(std::move(result).error().add_note(
                    "while running `" + format_command(action.argv) + "`"
                ));
            }
            if (!result->succeeded()) {
                return std::unexpected(error(
                    "`" + action.description + "` exited with code "
                        + std::to_string(result->exit_code)
                ).add_note("command: " + format_command(action.argv)));
            }
            return {};
        }
    }

    bool CheckReport::requires_synchronization() const noexcept {
        return std::ranges::any_of(generated_files, [](const GeneratedFileCheck& file) {
            return file.state != GeneratedFileState::current;
        }) || std::ranges::any_of(actions, [](const ActionCheck& action) {
            return action.stage == ActionStage::synchronize
                && action.state == ActionState::required;
        });
    }

    Result<CheckReport> check(const BuildPlan& plan) {
        CheckReport report;
        std::vector<std::filesystem::path> changed;
        report.generated_files.reserve(plan.generated_files().size());
        for (const GeneratedFile& generated: plan.generated_files()) {
            auto state = generated_file_state(generated);
            if (!state)
                return std::unexpected(state.error());

            report.generated_files.push_back({generated.path, *state});
            if (*state != GeneratedFileState::current)
                changed.push_back(generated.path);
        }

        report.actions.reserve(plan.actions().size());
        for (const Action& action: plan.actions()) {
            auto state = action_state(action);
            if (!state)
                return std::unexpected(state.error());

            if (*state == ActionState::current && consumes_changed_path(action, changed))
                *state = ActionState::required;

            report.actions.push_back({action.description, *state, action.stage});
            if (action.stage == ActionStage::synchronize && *state != ActionState::current)
                append_changed_outputs(changed, action);
        }
        return report;
    }

    Result<GenerationReport> generate(const BuildPlan& plan) {
        GenerationReport report;
        auto state = check(plan);
        if (!state)
            return std::unexpected(state.error());

        for (std::size_t index = 0; index < plan.generated_files().size(); ++index) {
            const GeneratedFile& generated = plan.generated_files()[index];
            if (state->generated_files[index].state == GeneratedFileState::current) {
                ++report.unchanged;
                continue;
            }

            auto written = write_file(generated.path, generated.content);
            if (!written)
                return std::unexpected(written.error());

            ++report.written;
        }

        for (std::size_t index = 0; index < plan.actions().size(); ++index) {
            const Action& action = plan.actions()[index];
            if (action.stage != ActionStage::synchronize
                || state->actions[index].state == ActionState::current) {
                continue;
            }

            auto executed = execute_action(action);
            if (!executed)
                return std::unexpected(executed.error());

            ++report.synchronized;
        }
        return report;
    }

    Result<ExecutionReport> execute_actions(const BuildPlan& plan, const ActionStage stage) {
        ExecutionReport report;

        for (const Action& action: plan.actions()) {
            if (action.stage != stage)
                continue;

            auto executed = execute_action(action);
            if (!executed)
                return std::unexpected(executed.error());

            ++report.executed;
        }
        return report;
    }

    Result<ExecutionReport> execute(const BuildPlan& plan) {
        auto generated = generate(plan);
        if (!generated)
            return std::unexpected(generated.error());

        auto built = execute_actions(plan, ActionStage::build);
        if (!built)
            return std::unexpected(built.error());

        built->executed += generated->synchronized;
        return built;
    }

    Result<ExecutionReport> test(const BuildPlan& plan) {
        auto executed = execute(plan);
        if (!executed)
            return std::unexpected(executed.error());

        auto tested = execute_actions(plan, ActionStage::test);
        if (!tested)
            return std::unexpected(tested.error());

        executed->executed += tested->executed;
        return executed;
    }
}
