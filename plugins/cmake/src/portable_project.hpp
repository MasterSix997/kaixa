#pragma once

#include "configuration.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace kaixa::plugin::cmake::detail {
    struct PortableProject {
        PackageId package;
        std::filesystem::path source;
        bool exclude_from_all = false;
    };

    [[nodiscard]] Result<std::string> generate_portable_dependencies(
        const Graph& graph,
        const PackageNode& package,
        const Options& options,
        std::span<const PortableProject> projects
    );
}
