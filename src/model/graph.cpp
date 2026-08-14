#include <kaixa/model/graph.hpp>

#include <algorithm>
#include <functional>
#include <utility>

namespace kaixa {
    PackageId Graph::add(PackageNode node) {
        node.id = PackageId{m_nodes.size()};
        m_nodes.push_back(std::move(node));
        if (m_nodes.size() == 1)
            m_root = m_nodes.front().id;
        return m_nodes.back().id;
    }

    const PackageNode& Graph::operator[](const PackageId id) const {
        return m_nodes[id.index];
    }

    PackageNode& Graph::operator[](const PackageId id) {
        return m_nodes[id.index];
    }

    std::optional<PackageId> Graph::find_by_directory(const std::filesystem::path& directory) const {
        const auto found = std::ranges::find_if(m_nodes, [&directory](const PackageNode& node) {
            return node.directory == directory;
        });
        return found == m_nodes.end() ? std::nullopt : std::optional(found->id);
    }

    std::optional<PackageId> Graph::find_by_name(const std::string_view name) const {
        const auto found = std::ranges::find_if(m_nodes, [name](const PackageNode& node) {
            return node.name == name;
        });
        return found == m_nodes.end() ? std::nullopt : std::optional(found->id);
    }

    Result<std::vector<PackageId>> Graph::build_order() const {
        enum class State {
            unseen,
            visiting,
            complete
        };

        std::vector<State> states(m_nodes.size(), State::unseen);
        std::vector<PackageId> stack;
        std::vector<PackageId> order;

        std::function<Result<void>(PackageId)> visit = [&](const PackageId id) -> Result<void> {
            if (states[id.index] == State::complete)
                return {};
            if (states[id.index] == State::visiting) {
                std::string cycle = "dependency cycle: ";
                const auto begin = std::ranges::find(stack, id);
                for (auto current = begin; current != stack.end(); ++current) {
                    if (current != begin)
                        cycle += " -> ";
                    cycle += m_nodes[current->index].name;
                }
                cycle += " -> ";
                cycle += m_nodes[id.index].name;
                return std::unexpected(error(std::move(cycle)));
            }

            states[id.index] = State::visiting;
            stack.push_back(id);
            for (const PackageId dependency: m_nodes[id.index].dependencies) {
                auto result = visit(dependency);
                if (!result)
                    return result;
            }
            for (const PackageTargetDependencies& dependencies: m_nodes[id.index].target_dependencies) {
                for (const PackageId dependency: dependencies.packages) {
                    auto result = visit(dependency);
                    if (!result)
                        return result;
                }
            }

            stack.pop_back();
            states[id.index] = State::complete;
            order.push_back(id);
            return {};
        };

        for (const PackageNode& node: m_nodes) {
            auto result = visit(node.id);
            if (!result)
                return std::unexpected(result.error());
        }
        return order;
    }
}
