#pragma once

#include <kaixa/foundation/diagnostic.hpp>
#include <kaixa/model/package.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kaixa {
    struct PackageDependencyEntry {
        PackageId package;
        std::size_t depth = 0;
        bool repeated = false;
    };

    class Graph {
    public:
        [[nodiscard]] PackageId add(PackageNode node);
        [[nodiscard]] const PackageNode& operator[](PackageId id) const;
        [[nodiscard]] PackageNode& operator[](PackageId id);

        [[nodiscard]] std::span<const PackageNode> nodes() const noexcept { return m_nodes; }
        [[nodiscard]] std::span<const PackageId> roots() const noexcept { return m_roots; }
        [[nodiscard]] std::size_t size() const noexcept { return m_nodes.size(); }
        [[nodiscard]] bool empty() const noexcept { return m_nodes.empty(); }
        [[nodiscard]] bool is_root(PackageId id) const noexcept;
        void add_root(PackageId id);

        [[nodiscard]] std::optional<PackageId> find_by_directory(const std::filesystem::path& directory) const;
        [[nodiscard]] std::optional<PackageId> find_by_name(std::string_view name) const;
        [[nodiscard]] std::vector<PackageDependencyEntry> dependency_tree(std::span<const PackageId> roots) const;
        [[nodiscard]] Result<std::vector<PackageId>> build_order() const;
        [[nodiscard]] Result<std::vector<PackageId>> build_order(std::span<const PackageId> roots) const;

    private:
        std::vector<PackageNode> m_nodes;
        std::vector<PackageId> m_roots;
        std::map<std::string, PackageId, std::less<>> m_packages_by_name;
        std::map<std::filesystem::path, PackageId> m_packages_by_directory;
    };
}
