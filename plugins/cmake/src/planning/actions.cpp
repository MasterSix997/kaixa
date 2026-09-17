#include "actions.hpp"

#include <discovery/file_api.hpp>
#include <planning/variants.hpp>

#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace kaixa::plugin::cmake::detail {
    namespace {
        std::string join_prefixes(const std::vector<std::filesystem::path>& prefixes) {
            std::string result;
            for (const std::filesystem::path& prefix: prefixes) {
                if (!result.empty())
                    result += ';';

                result += prefix.string();
            }
            return result;
        }
    }

    Result<Action> configure_action(const ConfigureActionContext& route) {
        const Graph& graph = route.graph;
        const PackageNode& package = route.package;
        const BuildContext& context = route.build;
        Action configure;
        configure.output = ProcessOutputMode::stream;
        configure.description = "configure " + package.name;
        configure.argv = {"cmake", "-S", route.projects[package.id.index]->source.string(), "-B", context.directory.string()};
        if (route.install)
            configure.argv.push_back("-DCMAKE_INSTALL_PREFIX=" + route.install->string());
        configure.argv.push_back("-DCMAKE_PROJECT_INCLUDE=" + route.integration_file.string());
        configure.inputs.push_back(context.metadata);
        configure.inputs.push_back(route.integration_file);
        configure.argv.push_back("-DKAIXA_CMAKE_PREFIX_PATH=" + join_prefixes(route.prefixes));
        if (graph.is_root(package.id)) {
            const std::string output = context.output.generic_string();
            if (context.project.runtime_output || context.project.library_output || context.project.archive_output)
                configure.argv.push_back("-DKAIXA_OUTPUT_ROOT=" + output);

            configure.argv.push_back("-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=" + output + "/bin/$<0:>");
            configure.argv.push_back("-DCMAKE_LIBRARY_OUTPUT_DIRECTORY=" + output + "/lib/$<0:>");
            configure.argv.push_back("-DCMAKE_ARCHIVE_OUTPUT_DIRECTORY=" + output + "/lib/$<0:>");
        }
        if (!uses_multiple_configurations(context.generator))
            configure.argv.push_back("-DCMAKE_BUILD_TYPE=" + context.configuration);

        if (context.build.generator && !requested_generator(context.build.configure_arguments)) {
            configure.argv.push_back("-G");
            configure.argv.push_back(*context.build.generator);
        }
        if (context.build.c_compiler)
            configure.argv.push_back("-DCMAKE_C_COMPILER=" + *context.build.c_compiler);

        if (context.build.cxx_compiler)
            configure.argv.push_back("-DCMAKE_CXX_COMPILER=" + *context.build.cxx_compiler);

        if (context.build.toolchain) {
            if (!std::filesystem::is_regular_file(*context.build.toolchain)) {
                return std::unexpected(error("CMake toolchain file does not exist: " + context.build.toolchain->string()));
            }
            configure.argv.push_back("-DCMAKE_TOOLCHAIN_FILE=" + context.build.toolchain->string());
        }
        if (exports_compile_commands(context.generator))
            configure.argv.push_back("-DCMAKE_EXPORT_COMPILE_COMMANDS=ON");

        configure.argv.insert(configure.argv.end(), context.build.configure_arguments.begin(), context.build.configure_arguments.end());
        configure.working_directory = package.directory;
        configure.package = package.id;
        configure.configured_artifact = route.instance.artifact;
        for (const PackageId id: route.source_packages)
            configure.inputs.push_back(route.projects[id.index]->cmakelists);

        configure.outputs.push_back(context.directory / "CMakeCache.txt");
        auto checked_state = configuration_state(context.directory, configure.inputs);
        configure.checked_state = route.reset ? ActionState::required : (checked_state ? *checked_state : ActionState::unknown);
        return configure;
    }

    std::optional<Action> compile_commands_action(
        const PackageNode& package,
        const BuildContext& context,
        const std::filesystem::path& workspace,
        const bool configuring
    ) {
        if (!exports_compile_commands(context.generator))
            return std::nullopt;

        const std::filesystem::path produced = context.directory / "compile_commands.json";
        std::error_code failure;
        if (!configuring && !std::filesystem::exists(produced, failure))
            return std::nullopt;
        const std::filesystem::path published = workspace / "compile_commands.json";
        Action publish;
        publish.description = "publish compile commands for " + package.name;
        publish.argv = {"cmake", "-E", "copy_if_different", produced.string(), published.string()};
        publish.working_directory = workspace;
        publish.inputs.push_back(produced);
        publish.outputs.push_back(published);
        publish.package = package.id;
        return publish;
    }

    void append_build_action(ExecutionPlan& plan, Action action, const bool installing) {
        // installing needs the artifact in place before the install step, so it synchronizes
        if (installing)
            plan.synchronize(std::move(action));
        else
            plan.build(std::move(action));
    }

    Action build_action(
        const PackageNode& package,
        const BuildContext& context,
        const ConfiguredPackageInstance& instance,
        const BuildRequest& request,
        const bool selected
    ) {
        Action build;
        build.output = ProcessOutputMode::stream;
        build.description = selected ? "build selected targets for " + package.name : "build " + package.name;
        build.argv = {"cmake", "--build", context.directory.string(), "--config", context.configuration};
        build.working_directory = package.directory;
        build.inputs.push_back(context.directory / "CMakeCache.txt");
        build.outputs.push_back(context.directory);
        build.package = package.id;
        build.configured_artifact = instance.artifact;
        if (selected) {
            build.argv.push_back("--target");
            build.argv.insert(build.argv.end(), request.targets.begin(), request.targets.end());
        }
        if (request.jobs) {
            build.argv.push_back("--parallel");
            build.argv.push_back(std::to_string(*request.jobs));
        }
        build.argv.insert(build.argv.end(), context.build.build_arguments.begin(), context.build.build_arguments.end());
        return build;
    }

    Action install_action(
        const PackageNode& package,
        const BuildContext& context,
        const ConfiguredPackageInstance& instance,
        const std::filesystem::path& destination
    ) {
        Action action;
        action.output = ProcessOutputMode::stream;
        action.description = "install " + package.name;
        action
            .argv = {"cmake", "--install", context.directory.string(), "--config", context.configuration, "--prefix", destination.string()};
        action.argv.insert(action.argv.end(), context.build.install_arguments.begin(), context.build.install_arguments.end());
        action.working_directory = package.directory;
        action.inputs.push_back(context.directory);
        action.outputs.push_back(destination);
        action.package = package.id;
        action.configured_artifact = instance.artifact;
        return action;
    }
}
