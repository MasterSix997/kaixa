#pragma once

#include <kaixa/foundation/diagnostic.hpp>

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kaixa {
    struct EnvironmentVariable {
        std::string name;
        std::string value;
    };

    enum class ProcessOutputMode {
        inherit,
        stream,
        capture
    };

    struct ProcessRequest {
        std::vector<std::string> argv;
        std::filesystem::path working_directory;
        std::vector<EnvironmentVariable> environment;
        ProcessOutputMode output = ProcessOutputMode::inherit;
    };

    struct ProcessResult {
        int exit_code = 0;
        std::string output;

        [[nodiscard]] bool succeeded() const noexcept { return exit_code == 0; }
    };

    [[nodiscard]] Result<ProcessResult> run_process(const ProcessRequest& request);
    [[nodiscard]] std::optional<std::string> environment_variable(std::string_view name);
    [[nodiscard]] std::string format_command(std::span<const std::string> argv);
}
