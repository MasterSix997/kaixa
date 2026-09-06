#pragma once

#include <model/project_model.hpp>

#include <kaixa/config/table_reader.hpp>
#include <kaixa/foundation/diagnostic.hpp>
#include <kaixa/model/graph.hpp>
#include <kaixa/model/policy.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace kaixa::plugin::cmake::detail {
    // What reading the declared project yields beyond the options themselves.
    struct ProjectSchema {
        std::optional<std::int64_t> default_standard;
    };

    // Reading the `[cmake]` table. This is the only phase that interprets a project's manifest
    // values; it creates no action and resolves no dependency, and its whole output is the typed
    // project model.
    [[nodiscard]] Result<ProjectSchema> read_declared_project(
        TableReader& options,
        Options& result,
        const PackageNode& package,
        const EffectivePolicy& package_policy
    );
    // A single declared target, read from an already-bound table. The assembly phase reuses it
    // for package targets, whose options it composes into the same document shape.
    [[nodiscard]] Result<TargetOptions> read_target(
        std::string name,
        TableReader& target,
        std::optional<std::int64_t> default_standard,
        const std::filesystem::path& source_root,
        const std::filesystem::path& output_root
    );
    [[nodiscard]] Result<void> read_declared_tests(TableReader& options, Options& result);
    [[nodiscard]] Result<void> read_dependency_modes(TableReader& options, Options& result, const Graph& graph, const PackageNode& package);
    [[nodiscard]] Result<BuildOptions> read_build_options(const Value* settings);
}
