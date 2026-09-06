#pragma once

#include <configuration.hpp>
#include <model/project_model.hpp>
#include <planning/variants.hpp>

#include <kaixa/build/plan.hpp>
#include <kaixa/extension/resolver.hpp>
#include <kaixa/model/graph.hpp>

#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace kaixa::plugin::cmake::detail {
    // One configured build tree, fully resolved: which project model it builds, with which build
    // options, into which directory. Planning produces it; generation, discovery and the action
    // builders all consume it and never look further back.
    struct BuildContext {
        Options project;
        BuildOptions build;
        std::optional<std::string> generator;
        BuildVariant variant;
        std::string configured_context;
        std::string configured_artifact;
        std::string configuration;
        std::filesystem::path directory;
        std::filesystem::path output;
        std::filesystem::path metadata;
    };

    struct PreparedProject {
        std::filesystem::path source;
        std::filesystem::path cmakelists;
    };

    // One package instance to plan for, and the request that selected it.
    struct ConfiguredRoute {
        std::string context;
        const ConfiguredPackageInstance* instance = nullptr;
        BuildRequest request;
    };

    struct RoutePlanningContext {
        const Graph& graph;
        const ExtensionRegistry& registry;
        const PackageNode& package;
        const BuildEnvironment& environment;
        std::span<const ConfiguredPackageInstance> instances;
        ExecutionPlan& plan;
        ConfigurationCache& cache;
    };

    [[nodiscard]] std::filesystem::path product_metadata_directory(const BuildContext& context);
}
