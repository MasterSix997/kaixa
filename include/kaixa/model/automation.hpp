#pragma once

#include <kaixa/foundation/diagnostic.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace kaixa {
    struct TaskDeclaration {
        std::string name;
        std::optional<std::string> package;
        std::vector<std::string> run;
        std::optional<std::filesystem::path> working_directory;
        std::map<std::string, std::string> environment;
        std::vector<std::filesystem::path> inputs;
        std::vector<std::filesystem::path> outputs;
        std::vector<std::string> after;
        std::filesystem::path source;
        SourceLocation location;
    };

    struct WorkflowDeclaration {
        std::string name;
        std::optional<std::string> package;
        std::vector<std::string> steps;
        std::filesystem::path source;
        SourceLocation location;
    };

    struct AutomationDocument {
        std::vector<TaskDeclaration> commands;
        std::vector<WorkflowDeclaration> workflows;
    };
}
