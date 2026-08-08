#pragma once

#include <kaixa/build/action.hpp>

#include <span>
#include <utility>

namespace kaixa {
    class BuildPlan {
    public:
        void add(Action action) { m_actions.push_back(std::move(action)); }
        [[nodiscard]] std::span<const Action> actions() const noexcept { return m_actions; }
        [[nodiscard]] bool empty() const noexcept { return m_actions.empty(); }

    private:
        std::vector<Action> m_actions;
    };
}
