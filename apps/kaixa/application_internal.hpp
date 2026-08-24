#pragma once

#include "application.hpp"
#include "configuration_output.hpp"

#include <kaixa/kaixa.hpp>
#include <kaixa/package/manager.hpp>
#include <kaixa/plugin/bundle.hpp>

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kaixa::cli::detail {
    struct Workspace {
        Graph graph;
        ManifestTreeSummary manifest_tree;
        std::vector<ConfiguredPackageInstance> instances;
        BuildEnvironment environment;
        ExtensionRegistry registry;
        std::vector<ConfigurationSource> configuration_sources;
        bool lock_changed = false;
    };

    int fail(const Diagnostic& diagnostic);

    std::optional<std::filesystem::path> user_configuration_path();

    Result<Workspace> open_workspace(
        const WorkspaceOptions& options,
        std::span<const std::string> unlocked_packages = {},
        bool unlock_all = false,
        bool write_lock = true,
        bool refresh_sources = true
    );

    Result<std::filesystem::path> selected_manifest(const WorkspaceOptions& options);
    Result<PackageId> require_single_root(const Graph& graph, std::string_view operation);

    void print_edit(const ManifestEdit& edit);

    void print_package(const Graph& graph, PackageId id, int depth, bool verbose = false);
    void print_providers(const ExtensionRegistry& registry);
    std::string_view state_name(GeneratedFileState state);
    std::string_view state_name(ActionState state);
    std::string_view stage_name(ActionStage stage);
    bool is_inside(const std::filesystem::path& path, const std::filesystem::path& directory);
    std::string display_path(const std::filesystem::path& path, const std::filesystem::path& workspace);
    void print_configuration_path(std::string_view name, const std::filesystem::path& path, const std::filesystem::path& workspace);
    Result<void> print_actions(const BuildPlan& plan, bool synchronization_only = false);
    void print_outputs(const BuildPlan& plan, const std::filesystem::path& workspace);
    void inspect_outputs(const Graph& graph, const BuildPlan& plan, const std::filesystem::path& workspace);
    Result<void> inspect_actions(const Graph& graph, const BuildPlan& plan, const std::filesystem::path& workspace, bool verbose);
    std::string_view product_purpose_name(ProductPurpose purpose);
    void print_products(
        std::span<const BuildProduct> products,
        const std::filesystem::path& workspace,
        std::span<const std::string> selected = {},
        bool filter = false
    );
    Result<void> inspect_effective_targets(const Graph& graph, const BuildEnvironment& environment, bool verbose);

    int run(const SearchCommand& command);
    int run(const InfoCommand& command);
    int run(const AddCommand& command);
    int run(const RemoveCommand& command);
    int run(const UpdateCommand& command);
    int run(const PublishCommand& command);
    int run(const BuildCommand& command);
    int run(const InstallCommand& command);
    int run(const TestCommand& command);
    int run(const BenchCommand& command);
    int run(const RunCommand& command);
    int run(const TaskCommand& command);
    int run(const WorkflowCommand& command);
}
