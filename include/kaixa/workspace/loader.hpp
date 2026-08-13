#pragma once

#include <kaixa/foundation/diagnostic.hpp>
#include <kaixa/model/graph.hpp>

#include <filesystem>

namespace kaixa {
    [[nodiscard]] Result<std::filesystem::path> find_manifest(const std::filesystem::path& start);
    [[nodiscard]] Result<Graph> load_workspace(const std::filesystem::path& start);
}
