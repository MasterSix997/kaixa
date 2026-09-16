#include <kaixa/build/executor.hpp>

#include <kaixa/foundation/filesystem.hpp>
#include <kaixa/foundation/process.hpp>

#include <algorithm>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <tuple>
#include <vector>

namespace kaixa {
    namespace {
        bool same_path(const std::filesystem::path& left, const std::filesystem::path& right) {
            return left.lexically_normal() == right.lexically_normal();
        }

        bool consumes_changed_path(const Action& action, const std::vector<std::filesystem::path>& changed) {
            return std::ranges::any_of(action.inputs, [&](const std::filesystem::path& input) {
                return std::ranges::any_of(changed, [&](const std::filesystem::path& path) { return same_path(input, path); });
            });
        }

        void append_changed_outputs(std::vector<std::filesystem::path>& changed, const Action& action) {
            for (const std::filesystem::path& output: action.outputs) {
                if (std::ranges::none_of(changed, [&](const std::filesystem::path& path) { return same_path(output, path); })) {
                    changed.push_back(output);
                }
            }
        }

        Result<GeneratedFileState> generated_file_state(const GeneratedFile& generated) {
            std::error_code failure;
            const bool exists = std::filesystem::exists(generated.path, failure);
            if (failure) {
                return std::unexpected(error("cannot inspect generated file `" + generated.path.string() + "`: " + failure.message()));
            }
            if (!exists)
                return GeneratedFileState::missing;

            if (!std::filesystem::is_regular_file(generated.path, failure) || failure)
                return GeneratedFileState::different;

            auto content = read_file(generated.path);
            if (!content)
                return std::unexpected(content.error());

            return *content == generated.content ? GeneratedFileState::current : GeneratedFileState::different;
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
                        return std::unexpected(error("cannot inspect action output `" + output.string() + "`: " + failure.message()));
                    }
                    return ActionState::required;
                }
                if (!std::filesystem::is_regular_file(output, failure) || failure)
                    return ActionState::unknown;

                const auto modified = std::filesystem::last_write_time(output, failure);
                if (failure) {
                    return std::unexpected(error("cannot inspect action output `" + output.string() + "`: " + failure.message()));
                }
                if (!oldest_output || modified < *oldest_output)
                    oldest_output = modified;
            }

            for (const std::filesystem::path& input: action.inputs) {
                std::error_code failure;
                if (!std::filesystem::exists(input, failure)) {
                    if (failure) {
                        return std::unexpected(error("cannot inspect action input `" + input.string() + "`: " + failure.message()));
                    }
                    return ActionState::required;
                }
                if (!std::filesystem::is_regular_file(input, failure) || failure)
                    return ActionState::unknown;

                const auto modified = std::filesystem::last_write_time(input, failure);
                if (failure) {
                    return std::unexpected(error("cannot inspect action input `" + input.string() + "`: " + failure.message()));
                }
                if (oldest_output && modified > *oldest_output)
                    return ActionState::unknown;
            }
            return ActionState::current;
        }

        std::string indented_tool_output(const std::string& captured) {
            std::string output = "tool output:";
            std::size_t start = 0;
            while (start < captured.size()) {
                const std::size_t end = captured.find('\n', start);
                const std::string_view line = end == std::string::npos ? std::string_view(captured).substr(start)
                                                                       : std::string_view(captured).substr(start, end - start);
                output += "\n    ";
                if (!line.empty() && line.back() == '\r')
                    output.append(line.substr(0, line.size() - 1));
                else
                    output += line;
                start = end == std::string::npos ? captured.size() : end + 1;
            }
            return output;
        }

        Result<ProcessResult> execute_action(const Action& action) {
            const ProcessRequest request{action.argv, action.working_directory, action.environment, action.output};
            auto result = run_process(request);
            if (!result) {
                return std::unexpected(std::move(result).error().add_note("while running `" + format_command(action.argv) + "`"));
            }
            if (!result->succeeded()) {
                Diagnostic diagnostic = error(
                    "action `" + action.description + "` failed (exit code " + std::to_string(result->exit_code) + ")"
                );
                if (action.output == ProcessOutputMode::capture && !result->output.empty())
                    diagnostic.notes.push_back(indented_tool_output(result->output));

                return std::unexpected(std::move(diagnostic));
            }
            return std::move(*result);
        }
    }

    bool CheckReport::requires_synchronization() const noexcept {
        return std::ranges::any_of(generated_files, [](const GeneratedFileCheck& file) {
            return file.state != GeneratedFileState::current;
        }) || std::ranges::any_of(synchronization, [](const ActionCheck& action) { return action.state == ActionState::required; });
    }

    std::span<const ActionCheck> CheckReport::phase(const ExecutionPhase phase) const noexcept {
        switch (phase) {
        case ExecutionPhase::synchronize: return synchronization;
        case ExecutionPhase::build: return build;
        case ExecutionPhase::task: return tasks;
        case ExecutionPhase::test: return tests;
        }
        return {};
    }

    Result<CheckReport> check(const ExecutionPlan& plan) {
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

        const auto check_phase =
            [&](const std::span<const Action> actions, std::vector<ActionCheck>& checks, const bool propagates) -> Result<void> {
            checks.reserve(actions.size());
            for (const Action& action: actions) {
                auto state = action_state(action);
                if (!state)
                    return std::unexpected(state.error());

                if (*state == ActionState::current && consumes_changed_path(action, changed))
                    *state = ActionState::required;

                checks.push_back({action.description, *state});
                if (propagates && *state != ActionState::current)
                    append_changed_outputs(changed, action);
            }
            return {};
        };

        for (
            const auto& [actions, checks, propagates]: {std::tuple{plan.synchronization(), &report.synchronization, true},
                std::tuple{plan.builds(), &report.build, false},
                std::tuple{plan.tasks(), &report.tasks, false},
                std::tuple{plan.tests(), &report.tests, false}}
        ) {
            auto checked = check_phase(actions, *checks, propagates);
            if (!checked)
                return std::unexpected(checked.error());
        }
        return report;
    }

    Result<GenerationReport> generate(const ExecutionPlan& plan) {
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

        for (std::size_t index = 0; index < plan.synchronization().size(); ++index) {
            if (state->synchronization[index].state == ActionState::current)
                continue;

            auto executed = execute_action(plan.synchronization()[index]);
            if (!executed)
                return std::unexpected(executed.error());

            ++report.synchronized;
        }
        return report;
    }

    Result<ExecutionReport> execute_actions(const ExecutionPlan& plan, const ExecutionPhase phase) {
        ExecutionReport report;
        std::span<const Action> actions;
        switch (phase) {
        case ExecutionPhase::synchronize: actions = plan.synchronization(); break;
        case ExecutionPhase::build: actions = plan.builds(); break;
        case ExecutionPhase::task: actions = plan.tasks(); break;
        case ExecutionPhase::test: actions = plan.tests(); break;
        }

        for (const Action& action: actions) {
            if (phase == ExecutionPhase::task) {
                auto state = action_state(action);
                if (!state)
                    return std::unexpected(state.error());

                if (*state == ActionState::current)
                    continue;
            }

            auto executed = execute_action(action);
            if (!executed)
                return std::unexpected(executed.error());

            ++report.executed;
            if (action.output == ProcessOutputMode::capture && !executed->output.empty()) {
                report.captured_outputs.push_back({action.description, std::move(executed->output)});
            }
        }
        return report;
    }

    Result<ExecutionReport> execute(const ExecutionPlan& plan) {
        auto generated = generate(plan);
        if (!generated)
            return std::unexpected(generated.error());

        auto built = execute_actions(plan, ExecutionPhase::build);
        if (!built)
            return std::unexpected(built.error());

        auto tasks = execute_actions(plan, ExecutionPhase::task);
        if (!tasks)
            return std::unexpected(tasks.error());

        built->executed += tasks->executed;
        built->executed += generated->synchronized;
        built->captured_outputs.insert(
            built->captured_outputs.end(),
            std::make_move_iterator(tasks->captured_outputs.begin()),
            std::make_move_iterator(tasks->captured_outputs.end())
        );
        return built;
    }

    Result<ExecutionReport> test(const ExecutionPlan& plan) {
        auto executed = execute(plan);
        if (!executed)
            return std::unexpected(executed.error());

        auto tested = execute_actions(plan, ExecutionPhase::test);
        if (!tested)
            return std::unexpected(tested.error());

        executed->executed += tested->executed;
        executed->captured_outputs.insert(
            executed->captured_outputs.end(),
            std::make_move_iterator(tested->captured_outputs.begin()),
            std::make_move_iterator(tested->captured_outputs.end())
        );
        return executed;
    }
}
