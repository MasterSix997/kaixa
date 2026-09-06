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

    // The phase an action belongs to is the container it was put in, not a field it carries: a
    // test action cannot end up in the build phase by mistake. Order inside a phase is the
    // contract; the phases themselves always run synchronize, build, task, test.
    class ExecutionPlan {
    public:
        void generate(GeneratedFile file) { m_files.push_back(std::move(file)); }
        void synchronize(Action action) { m_synchronization.push_back(std::move(action)); }
        void build(Action action) { m_build.push_back(std::move(action)); }
        void task(Action action) { m_tasks.push_back(std::move(action)); }
        void test(Action action) { m_tests.push_back(std::move(action)); }
        void output(BuildOutput output) { m_outputs.push_back(std::move(output)); }

        void append(ExecutionPlan plan) {
            move_into(m_files, std::move(plan.m_files));
            move_into(m_synchronization, std::move(plan.m_synchronization));
            move_into(m_build, std::move(plan.m_build));
            move_into(m_tasks, std::move(plan.m_tasks));
            move_into(m_tests, std::move(plan.m_tests));
            move_into(m_outputs, std::move(plan.m_outputs));
        }

        [[nodiscard]] std::span<const GeneratedFile> generated_files() const noexcept { return m_files; }
        [[nodiscard]] std::span<const Action> synchronization() const noexcept { return m_synchronization; }
        [[nodiscard]] std::span<Action> synchronization() noexcept { return m_synchronization; }
        [[nodiscard]] std::span<const Action> builds() const noexcept { return m_build; }
        [[nodiscard]] std::span<Action> builds() noexcept { return m_build; }
        [[nodiscard]] std::span<const Action> tasks() const noexcept { return m_tasks; }
        [[nodiscard]] std::span<Action> tasks() noexcept { return m_tasks; }
        [[nodiscard]] std::span<const Action> tests() const noexcept { return m_tests; }
        [[nodiscard]] std::span<Action> tests() noexcept { return m_tests; }
        [[nodiscard]] std::span<const BuildOutput> outputs() const noexcept { return m_outputs; }

        [[nodiscard]] std::size_t action_count() const noexcept {
            return m_synchronization.size() + m_build.size() + m_tasks.size() + m_tests.size();
        }

        [[nodiscard]] bool empty() const noexcept { return m_files.empty() && action_count() == 0 && m_outputs.empty(); }

    private:
        template <typename T> static void move_into(std::vector<T>& destination, std::vector<T> source) {
            destination.insert(destination.end(), std::make_move_iterator(source.begin()), std::make_move_iterator(source.end()));
        }

        std::vector<GeneratedFile> m_files;
        std::vector<Action> m_synchronization;
        std::vector<Action> m_build;
        std::vector<Action> m_tasks;
        std::vector<Action> m_tests;
        std::vector<BuildOutput> m_outputs;
    };
}
