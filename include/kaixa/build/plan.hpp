#pragma once

#include <kaixa/build/action.hpp>
#include <kaixa/model/package.hpp>

#include <filesystem>
#include <iterator>
#include <optional>
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
        std::optional<std::filesystem::path> build_directory;
        std::optional<std::string> configured_artifact;
    };

    class BuildPlan {
    public:
        void generate(GeneratedFile file) { m_files.push_back(std::move(file)); }
        void add(Action action) { m_actions.push_back(std::move(action)); }
        void output(BuildOutput output) { m_outputs.push_back(std::move(output)); }
        void append(BuildPlan plan) {
            m_files.insert(m_files.end(), std::make_move_iterator(plan.m_files.begin()), std::make_move_iterator(plan.m_files.end()));
            m_actions
                .insert(m_actions.end(), std::make_move_iterator(plan.m_actions.begin()), std::make_move_iterator(plan.m_actions.end()));
            m_outputs
                .insert(m_outputs.end(), std::make_move_iterator(plan.m_outputs.begin()), std::make_move_iterator(plan.m_outputs.end()));
        }

        [[nodiscard]] std::span<const GeneratedFile> generated_files() const noexcept { return m_files; }
        [[nodiscard]] std::span<Action> actions() noexcept { return m_actions; }
        [[nodiscard]] std::span<const Action> actions() const noexcept { return m_actions; }
        [[nodiscard]] std::span<const BuildOutput> outputs() const noexcept { return m_outputs; }
        [[nodiscard]] bool empty() const noexcept { return m_files.empty() && m_actions.empty() && m_outputs.empty(); }

    private:
        std::vector<GeneratedFile> m_files;
        std::vector<Action> m_actions;
        std::vector<BuildOutput> m_outputs;
    };
}
