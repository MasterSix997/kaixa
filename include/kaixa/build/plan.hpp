#pragma once

#include <kaixa/build/action.hpp>
#include <kaixa/model/package.hpp>

#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace kaixa {
    struct GeneratedFile {
        std::filesystem::path path;
        std::string content;
    };

    struct BuildOutput {
        PackageId package;
        std::string resolver;
        std::filesystem::path path;
    };

    class BuildPlan {
    public:
        void generate(GeneratedFile file) { m_files.push_back(std::move(file)); }
        void add(Action action) { m_actions.push_back(std::move(action)); }
        void output(BuildOutput output) { m_outputs.push_back(std::move(output)); }

        [[nodiscard]] std::span<const GeneratedFile> generated_files() const noexcept {
            return m_files;
        }
        [[nodiscard]] std::span<Action> actions() noexcept { return m_actions; }
        [[nodiscard]] std::span<const Action> actions() const noexcept { return m_actions; }
        [[nodiscard]] std::span<const BuildOutput> outputs() const noexcept { return m_outputs; }
        [[nodiscard]] bool empty() const noexcept {
            return m_files.empty() && m_actions.empty() && m_outputs.empty();
        }

    private:
        std::vector<GeneratedFile> m_files;
        std::vector<Action> m_actions;
        std::vector<BuildOutput> m_outputs;
    };
}
