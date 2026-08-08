#pragma once

#include <kaixa/foundation/diagnostic.hpp>
#include <kaixa/model/package.hpp>

#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace kaixa {
    class Graph {
    public:
        [[nodiscard]] PackageId add(PackageNode node);
        [[nodiscard]] const PackageNode& operator[](PackageId id) const;
        [[nodiscard]] PackageNode& operator[](PackageId id);

        [[nodiscard]] std::span<const PackageNode> nodes() const noexcept { return m_nodes; }
        [[nodiscard]] std::size_t size() const noexcept { return m_nodes.size(); }
        [[nodiscard]] bool empty() const noexcept { return m_nodes.empty(); }
        [[nodiscard]] PackageId root() const noexcept { return m_root; }

        [[nodiscard]] std::optional<PackageId> find_by_directory(
            const std::filesystem::path& directory
        ) const;
        [[nodiscard]] std::optional<PackageId> find_by_name(std::string_view name) const;
        [[nodiscard]] Result<std::vector<PackageId>> build_order() const;

    private:
        std::vector<PackageNode> m_nodes;
        PackageId m_root;
    };
}
