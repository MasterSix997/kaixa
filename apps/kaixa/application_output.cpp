#include "application_internal.hpp"

#include <kaixa/foundation/process.hpp>
#include <kaixa/model/effective_product.hpp>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

namespace kaixa::cli::detail {
    namespace {
        void print_package(std::ostream& output, const Graph& graph, const PackageDependencyEntry& entry, const bool verbose) {
            const PackageNode& package = graph[entry.package];
            output << std::string(entry.depth * 2, ' ') << package.name;
            if (package.kind == PackageKind::opaque)
                output << " (opaque)";
            else
                output << " (" << package.resolver << ')';

            if (entry.repeated) {
                output << " [already shown]\n";
                return;
            }

            if (verbose)
                output << " -> " << package.directory.string();

            output << '\n';
            if (verbose && package.source) {
                output << std::string((entry.depth + 1) * 2, ' ') << "source: ";
                if (package.source->locator)
                    output << package.source->locator->driver;
                else
                    output << "provider descriptor";

                output << ", authority: " << package.source->authority;
                if (package.source->provider)
                    output << ", provider: " << *package.source->provider;

                if (package.source->identity)
                    output << ", identity: " << *package.source->identity;

                output << '\n';
            }
            if (verbose && !package.active_features.empty()) {
                output << std::string((entry.depth + 1) * 2, ' ') << "features:";
                for (const std::string& feature: package.active_features)
                    output << ' ' << feature;

                output << '\n';
            }
        }
    }

    void print_packages(std::ostream& output, const Graph& graph, const std::span<const PackageId> roots, const bool verbose) {
        for (const PackageDependencyEntry& entry: graph.dependency_tree(roots))
            print_package(output, graph, entry, verbose);
    }

    void print_providers(const ExtensionRegistry& registry) {
        if (registry.providers().empty())
            return;

        std::cout << "providers:\n";
        for (const auto& provider: registry.providers()) {
            const ProviderInfo info = provider->info();
            std::cout << "  " << info.name << ": " << info.driver;
            if (info.is_default)
                std::cout << " (default)";

            std::cout << '\n';
        }
    }

    std::string_view state_name(const GeneratedFileState state) {
        switch (state) {
        case GeneratedFileState::current: return "current";
        case GeneratedFileState::missing: return "missing";
        case GeneratedFileState::different: return "different";
        }
        return "unknown";
    }

    std::string_view state_name(const ActionState state) {
        switch (state) {
        case ActionState::current: return "current";
        case ActionState::required: return "required";
        case ActionState::unknown: return "unknown";
        }
        return "unknown";
    }

    std::string_view stage_name(const ActionStage stage) {
        switch (stage) {
        case ActionStage::synchronize: return "synchronize";
        case ActionStage::build: return "build";
        case ActionStage::task: return "task";
        case ActionStage::test: return "test";
        }
        return "action";
    }

    bool is_inside(const std::filesystem::path& path, const std::filesystem::path& directory) {
        const std::filesystem::path relative = path.lexically_relative(directory);
        return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
    }

    std::string display_path(const std::filesystem::path& path, const std::filesystem::path& workspace) {
        if (is_inside(path, workspace)) {
            return path.lexically_relative(workspace).generic_string();
        }

        return path.string();
    }

    bool path_exists(const std::filesystem::path& path) {
        std::error_code failure;
        return std::filesystem::is_regular_file(path, failure) && !failure;
    }

    void print_configuration_path(const std::string_view name, const std::filesystem::path& path, const std::filesystem::path& workspace) {
        std::cout << name << ": " << display_path(path, workspace) << (path_exists(path) ? " [present]" : " [missing]") << '\n';
    }

    Result<void> print_actions(const BuildPlan& plan, const bool synchronization_only) {
        auto state = check(plan);
        if (!state)
            return std::unexpected(state.error());

        for (std::size_t index = 0; index < plan.actions().size(); ++index) {
            const Action& action = plan.actions()[index];
            if (synchronization_only && action.stage != ActionStage::synchronize)
                continue;

            if ((action.stage == ActionStage::synchronize || action.stage == ActionStage::task)
                && state->actions[index].state == ActionState::current) {
                continue;
            }

            // Keep the normal command output at the Kaixa action level.  The
            // concrete argv is available from `inspect actions --verbose`;
            // printing it here made resolver, CMake and shell diagnostics
            // appear as one confusing stream.
            std::cout << action.description << '\n';
        }

        std::cout.flush();
        return {};
    }

    void print_outputs(const BuildPlan& plan, const std::filesystem::path& workspace) {
        for (const BuildOutput& output: plan.outputs())
            std::cout << "artifact: " << display_path(output.path, workspace) << '\n';
    }

    void inspect_outputs(const Graph& graph, const BuildPlan& plan, const std::filesystem::path& workspace) {
        if (plan.outputs().empty()) {
            std::cout << "no build outputs\n";
            return;
        }

        for (const BuildOutput& output: plan.outputs()) {
            std::cout << output.resolver << ' ' << graph[output.package].name << " -> " << display_path(output.path, workspace) << '\n';
        }
    }

    Result<void> inspect_actions(const Graph& graph, const BuildPlan& plan, const std::filesystem::path& workspace, const bool verbose) {
        auto report = check(plan);
        if (!report)
            return std::unexpected(report.error());

        if (plan.actions().empty()) {
            std::cout << "no build actions\n";
            return {};
        }

        for (std::size_t index = 0; index < plan.actions().size(); ++index) {
            const Action& action = plan.actions()[index];
            const ActionCheck& checked = report->actions[index];
            std::cout << state_name(checked.state) << ' ' << stage_name(action.stage) << ' ';
            if (action.package)
                std::cout << graph[*action.package].name << ": ";

            std::cout << action.description << '\n';
            if (!verbose)
                continue;

            std::cout << "  command: " << format_command(action.argv) << '\n';
            std::cout << "  working directory: " << display_path(action.working_directory, workspace) << '\n';
            if (action.configured_artifact)
                std::cout << "  configured artifact: " << *action.configured_artifact << '\n';

            for (const std::filesystem::path& input: action.inputs)
                std::cout << "  input: " << display_path(input, workspace) << '\n';

            for (const std::filesystem::path& output: action.outputs)
                std::cout << "  output: " << display_path(output, workspace) << '\n';
        }
        return {};
    }

    std::string_view product_kind_name(const ProductKind kind) {
        switch (kind) {
        case ProductKind::executable: return "executable";
        case ProductKind::static_library: return "static-library";
        case ProductKind::shared_library: return "shared-library";
        case ProductKind::module_library: return "module-library";
        case ProductKind::object_library: return "object-library";
        case ProductKind::interface_library: return "interface-library";
        case ProductKind::utility: return "utility";
        }
        return "product";
    }

    std::string_view product_purpose_name(const ProductPurpose purpose) {
        switch (purpose) {
        case ProductPurpose::primary: return "primary";
        case ProductPurpose::test: return "test";
        case ProductPurpose::example: return "example";
        case ProductPurpose::benchmark: return "benchmark";
        }
        return "product";
    }

    void print_products(
        const std::span<const BuildProduct> products,
        const std::filesystem::path& workspace,
        const std::span<const std::string> selected,
        const bool filter
    ) {
        for (const BuildProduct& product: products) {
            if (filter && std::ranges::find(selected, product.name) == selected.end())
                continue;

            if (product.purpose != ProductPurpose::primary)
                std::cout << product_purpose_name(product.purpose) << ' ';

            std::cout << product_kind_name(product.kind) << ' ' << product.name;
            if (product.artifact)
                std::cout << " -> " << display_path(*product.artifact, workspace);

            std::cout << '\n';
        }
    }

    std::string_view effective_product_type_name(const EffectiveProductType type) {
        switch (type) {
        case EffectiveProductType::executable: return "executable";
        case EffectiveProductType::static_library: return "static-library";
        case EffectiveProductType::shared_library: return "shared-library";
        case EffectiveProductType::interface_library: return "interface-library";
        }
        return "product";
    }

    std::string_view associated_target_kind_name(const PackageTargetKind kind) {
        switch (kind) {
        case PackageTargetKind::test: return "test";
        case PackageTargetKind::example: return "example";
        case PackageTargetKind::benchmark: return "benchmark";
        }
        return "target";
    }

    Result<void> inspect_effective_targets(const Graph& graph, const BuildEnvironment& environment, const bool verbose) {
        bool any = false;
        for (const PackageNode& node: graph.nodes()) {
            if (!node.manifest)
                continue;

            auto package = realize_package(graph, node.id, {environment.configuration.profile, host_target_os()});
            if (!package)
                return std::unexpected(package.error());

            if (package->products.empty() && package->targets.empty())
                continue;

            any = true;
            std::cout << node.name << ":\n";
            for (const EffectiveProduct& product: package->products) {
                std::cout << "  " << effective_product_type_name(product.type) << ' ' << product.name << '\n';
                if (!verbose)
                    continue;

                for (const std::filesystem::path& source: product.sources.files)
                    std::cout << "    source: " << source.generic_string() << '\n';

                for (const std::filesystem::path& source: product.dependency_source_files)
                    std::cout << "    dependency-source: " << source.generic_string() << '\n';

                for (const std::filesystem::path& header: product.public_headers.files)
                    std::cout << "    public-header: " << header.generic_string() << '\n';

                for (const TableEntry& definition: product.definitions)
                    std::cout << "    define: " << definition.key << '\n';

                for (const TableEntry& definition: product.public_definitions)
                    std::cout << "    public-define: " << definition.key << '\n';

                for (const std::filesystem::path& runtime_file: product.runtime_files.files)
                    std::cout << "    runtime-file: " << runtime_file.generic_string() << '\n';
            }
            for (const EffectiveTarget& target: package->targets) {
                std::cout << "  " << associated_target_kind_name(target.target.kind) << ' ' << target.target.name.value_or("<unnamed>");
                if (target.availability == TargetAvailability::skipped)
                    std::cout << " [skipped]";

                std::cout << '\n';
                for (const std::string& reason: target.skip_reasons)
                    std::cout << "    reason: " << reason << '\n';

                if (verbose) {
                    for (const std::filesystem::path& source: target.target.sources.files)
                        std::cout << "    source: " << source.generic_string() << '\n';

                    if (target.target.category)
                        std::cout << "    category: " << *target.target.category << '\n';

                    if (target.target.framework)
                        std::cout << "    adapter: " << *target.target.framework << '\n';
                }
            }
        }
        if (!any)
            std::cout << "no products or associated targets\n";

        return {};
    }

}
