#include <kaixa/build/executor.hpp>

#include <kaixa/foundation/process.hpp>

namespace kaixa {
    Result<ExecutionReport> execute(const BuildPlan& plan) {
        ExecutionReport report;
        for (const Action& action: plan.actions()) {
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
            ++report.executed;
        }
        return report;
    }
}
