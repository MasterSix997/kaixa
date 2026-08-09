#pragma once

#include <kaixa/build/action.hpp>

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

    class BuildPlan {
    public:
        void generate(GeneratedFile file) { m_files.push_back(std::move(file)); }
        void add(Action action) { m_actions.push_back(std::move(action)); }

        [[nodiscard]] std::span<const GeneratedFile> generated_files() const noexcept {
            return m_files;
        }
        [[nodiscard]] std::span<const Action> actions() const noexcept { return m_actions; }
        [[nodiscard]] bool empty() const noexcept {
            return m_files.empty() && m_actions.empty();
        }

    private:
        std::vector<GeneratedFile> m_files;
        std::vector<Action> m_actions;
    };
}
