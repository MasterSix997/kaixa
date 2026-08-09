#include <kaixa/build/executor.hpp>

#include <kaixa/foundation/process.hpp>

#include <fstream>
#include <system_error>

namespace kaixa {
    Result<ExecutionReport> execute(const BuildPlan& plan) {
        ExecutionReport report;

        for (const GeneratedFile& generated: plan.generated_files()) {
            std::error_code failure;
            std::filesystem::create_directories(generated.path.parent_path(), failure);
            if (failure) {
                return std::unexpected(error(
                    "cannot create directory for generated file `" + generated.path.string()
                        + "`: " + failure.message()
                ));
            }

            std::ofstream output(generated.path, std::ios::binary | std::ios::trunc);
            if (!output) {
                return std::unexpected(error(
                    "cannot write generated file `" + generated.path.string() + "`"
                ));
            }
            output.write(
                generated.content.data(),
                static_cast<std::streamsize>(generated.content.size())
            );
            if (!output) {
                return std::unexpected(error(
                    "cannot write generated file `" + generated.path.string() + "`"
                ));
            }
        }

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
