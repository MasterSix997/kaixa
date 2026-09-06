#include "project.hpp"
#include <generation/cmake_syntax.hpp>

#include <configuration.hpp>
#include <testing/testing.hpp>

#include <algorithm>
#include <filesystem>
#include <ranges>
#include <string>
#include <vector>

namespace kaixa::plugin::cmake::detail {
    namespace {
        std::string project_path(const Options& options, const std::string_view value) {
            if (value.starts_with("$<") || value.starts_with('<'))
                return std::string(value);

            const std::filesystem::path path(value);
            if (options.generation == GenerationMode::state)
                return (path.is_absolute() ? path : options.source / path).lexically_normal().generic_string();

            if (path.is_relative())
                return path.lexically_normal().generic_string();

            for (const PortableSourceRoot& root: options.portable_source_roots) {
                const std::filesystem::path relative = path.lexically_relative(root.directory);
                if (!relative.empty() && *relative.begin() != "..")
                    return "${" + root.variable + "}/" + relative.generic_string();
            }

            const std::filesystem::path relative = path.lexically_relative(options.source);
            return relative.empty() ? path.generic_string() : relative.generic_string();
        }

        std::string project_definition(const Options& options, const std::string& definition) {
            const std::size_t separator = definition.find('=');
            if (separator == std::string::npos)
                return definition;

            const std::string value = definition.substr(separator + 1);
            if (value.empty() || std::filesystem::path(value).is_relative())
                return definition;

            return definition.substr(0, separator + 1) + project_path(options, value);
        }

        std::vector<std::string> project_definitions(const Options& options, const std::vector<std::string>& values) {
            std::vector<std::string> result;
            result.reserve(values.size());
            for (const std::string& value: values)
                result.push_back(project_definition(options, value));

            return result;
        }

        std::string project_source_path(const Options& options, const std::filesystem::path& path) {
            std::string value = project_path(options, path.generic_string());
            if (options.generation == GenerationMode::export_project
                && !value.contains("${")
                && std::filesystem::path(value).is_relative()) {
                return "${CMAKE_CURRENT_LIST_DIR}/" + value;
            }

            return value;
        }

        std::string output_directory(const std::filesystem::path& path) {
            const std::string value = path.generic_string();
            return path.is_absolute() ? syntax::expanding_literal(value)
                                      : syntax::expanding_literal("${_kaixa_output_root}/" + value + "/$<0:>");
        }

        std::string project_version(const PackageNode& package) {
            if (!package.manifest() || !package.manifest()->version)
                return {};

            std::string value = package.manifest()->version->text;
            const std::size_t suffix = value.find_first_of("-+");
            if (suffix != std::string::npos)
                value.resize(suffix);

            if (value.empty() || value.front() == '.' || value.back() == '.')
                return {};

            if (!std::ranges::all_of(value, [](const char character) {
                    return (character >= '0' && character <= '9') || character == '.';
                })) {
                return {};
            }
            return value;
        }

        void emit_values(
            std::string& output,
            const std::string_view command,
            const std::string& target,
            const std::string_view scope,
            const std::vector<std::string>& values
        ) {
            if (values.empty())
                return;

            output += std::string(command) + "(" + target + " " + std::string(scope) + "\n";
            for (const std::string& value: values)
                output += "    " + syntax::argument(value) + "\n";

            output += ")\n\n";
        }

        std::vector<std::string> project_paths(const Options& options, const std::vector<std::string>& values) {
            std::vector<std::string> result;
            result.reserve(values.size());
            for (const std::string& value: values)
                result.push_back(project_path(options, value));

            return result;
        }

        std::vector<std::string> project_link_values(const Options& options, const std::vector<std::string>& values) {
            std::vector<std::string> result;
            result.reserve(values.size());
            for (const std::string& value: values) {
                const std::filesystem::path path(value);
                const std::filesystem::path relative = path.lexically_relative(options.source);
                if (path.is_absolute()
                    && std::ranges::distance(relative) == 1
                    && !relative.has_extension()
                    && !std::filesystem::exists(path)) {
                    result.push_back(relative.generic_string());
                } else {
                    result.push_back(path.is_absolute() ? project_path(options, value) : value);
                }
            }
            return result;
        }

        void generate_runtime_materialization(std::string& output, const Options& options, const TargetOptions& target) {
            for (const TargetOptions::RuntimeFile& runtime_file: target.runtime_files) {
                const std::string destination = "$<TARGET_FILE_DIR:" + target.name + ">/" + runtime_file.destination.generic_string();
                const std::filesystem::path parent = runtime_file.destination.parent_path();
                output += "add_custom_command(TARGET " + target.name + " POST_BUILD\n";
                output += "    COMMAND ${CMAKE_COMMAND} -E make_directory "
                    + syntax::literal("$<TARGET_FILE_DIR:" + target.name + ">/" + parent.generic_string())
                    + "\n";
                output += "    COMMAND ${CMAKE_COMMAND} -E "
                    + std::string(runtime_file.directory ? "copy_directory " : "copy_if_different ")
                    + syntax::argument(project_source_path(options, runtime_file.source))
                    + " "
                    + syntax::literal(destination)
                    + "\n"
                      ")\n\n";
            }
        }

        std::vector<std::string> generated_public_includes(const Options& options, const TargetOptions& target) {
            std::vector<std::string> result = project_paths(options, target.public_include_directories);
            if (!target.install || result.empty())
                return result;

            for (std::string& include: result) {
                const std::filesystem::path path = include;
                if (options.generation == GenerationMode::export_project
                    && !include.contains("${")
                    && !include.starts_with("$<")
                    && path.is_relative()) {
                    include.insert(0, "${CMAKE_CURRENT_LIST_DIR}/");
                }

                include.insert(0, "$<BUILD_INTERFACE:");
                include += '>';
            }
            result.push_back("$<INSTALL_INTERFACE:include>");
            return result;
        }

        std::vector<std::string> generated_public_system_includes(const Options& options, const TargetOptions& target) {
            std::vector<std::string> result = project_paths(options, target.public_system_include_directories);
            if (!target.install)
                return result;

            for (std::string& include: result) {
                const std::filesystem::path path = include;
                if (options.generation == GenerationMode::export_project
                    && !include.contains("${")
                    && !include.starts_with("$<")
                    && path.is_relative()) {
                    include.insert(0, "${CMAKE_CURRENT_LIST_DIR}/");
                }

                include.insert(0, "$<BUILD_INTERFACE:");
                include += '>';
            }
            return result;
        }

        void generate_target_install(
            std::string& output,
            const Options& options,
            const std::string_view package,
            const TargetOptions& target
        ) {
            if (!target.install)
                return;

            output += "install(TARGETS "
                + target.name
                + " EXPORT "
                + std::string(package)
                + "Targets\n"
                  "    RUNTIME DESTINATION bin\n"
                  "    LIBRARY DESTINATION lib\n"
                  "    ARCHIVE DESTINATION lib\n"
                  "    INCLUDES DESTINATION include\n"
                  ")\n\n";
            for (const TargetOptions::InstallHeader& header: target.install_headers) {
                output += "install(FILES "
                    + syntax::argument(project_source_path(options, header.source))
                    + " DESTINATION "
                    + syntax::literal((std::filesystem::path("include") / header.destination.parent_path()).generic_string());
                if (header.source.filename() != header.destination.filename())
                    output += " RENAME " + syntax::literal(header.destination.filename().generic_string());

                output += ")\n";
            }
            if (!target.install_headers.empty())
                output += '\n';

            for (const TargetOptions::RuntimeFile& runtime_file: target.runtime_files) {
                const std::filesystem::path destination = std::filesystem::path("bin") / runtime_file.destination.parent_path();
                if (runtime_file.directory) {
                    output += "install(DIRECTORY "
                        + syntax::argument(project_source_path(options, runtime_file.source) + "/")
                        + " DESTINATION "
                        + syntax::literal((destination / runtime_file.destination.filename()).generic_string())
                        + ")\n";
                } else {
                    output += "install(FILES "
                        + syntax::argument(project_source_path(options, runtime_file.source))
                        + " DESTINATION "
                        + syntax::literal(destination.generic_string());
                    if (runtime_file.source.filename() != runtime_file.destination.filename())
                        output += " RENAME " + syntax::literal(runtime_file.destination.filename().generic_string());

                    output += ")\n";
                }
            }
            if (!target.runtime_files.empty())
                output += '\n';
        }

        void generate_package_export(
            std::string& output,
            const std::string_view package,
            const std::string_view version,
            const std::span<const TargetOptions> targets,
            const std::span<const std::string> dependencies
        ) {
            if (std::ranges::none_of(targets, &TargetOptions::install))
                return;

            const std::string name(package);
            const std::string config = name + "Config.cmake";
            std::string config_content;
            if (!dependencies.empty()) {
                config_content += "include(CMakeFindDependencyMacro)\\n";
                for (const std::string& dependency: dependencies)
                    config_content += "find_dependency(" + dependency + " REQUIRED)\\n";
            }
            config_content += "include(\\\"\\${CMAKE_CURRENT_LIST_DIR}/" + name + "Targets.cmake\\\")\\n";
            output += "file(WRITE \"${CMAKE_CURRENT_BINARY_DIR}/" + config + "\" \"" + config_content + "\")\n";
            output += "install(EXPORT "
                + name
                + "Targets FILE "
                + name
                + "Targets.cmake NAMESPACE "
                + name
                + ":: DESTINATION lib/cmake/"
                + name
                + ")\n";
            if (!version.empty()) {
                output += "include(CMakePackageConfigHelpers)\n";
                output += "write_basic_package_version_file(\"${CMAKE_CURRENT_BINARY_DIR}/"
                    + name
                    + "ConfigVersion.cmake\" VERSION "
                    + std::string(version)
                    + " COMPATIBILITY SameMajorVersion)\n";
            }
            output += "install(FILES \"${CMAKE_CURRENT_BINARY_DIR}/" + config + "\"";
            if (!version.empty())
                output += " \"${CMAKE_CURRENT_BINARY_DIR}/" + name + "ConfigVersion.cmake\"";

            output += " DESTINATION lib/cmake/" + name + ")\n\n";
        }

        std::vector<std::string> project_languages(const std::span<const ProjectPackage> packages) {
            std::vector<std::string> result;
            for (const ProjectPackage& package: packages) {
                for (const std::string& language: package.options->languages) {
                    if (std::ranges::find(result, language) == result.end())
                        result.push_back(language);
                }
            }
            return result;
        }

        void generate_project_header(std::string& output, const std::span<const ProjectPackage> packages) {
            const PackageNode& owner = *packages.front().package;
            const std::string version = project_version(owner);
            output = std::string(generated_marker) + "\ncmake_minimum_required(VERSION 3.20)\nproject(" + owner.name;
            if (!version.empty())
                output += " VERSION " + version;

            output += " LANGUAGES";
            for (const std::string& language: project_languages(packages))
                output += " " + language;

            output += ")\n\n";
        }

        void generate_export_bootstrap(std::string& output, const Options& options) {
            if (options.generation != GenerationMode::export_project)
                return;

            if (options.msvc_runtime != MsvcRuntime::default_runtime) {
                const std::string runtime = options.msvc_runtime == MsvcRuntime::static_runtime ? "MultiThreaded" : "MultiThreadedDLL";
                output += "set(CMAKE_MSVC_RUNTIME_LIBRARY \"" + runtime + "$<$<CONFIG:Debug>:Debug>\")\n";
            }
            if (options.cxx_standard) {
                output += "set(CMAKE_CXX_STANDARD " + std::to_string(*options.cxx_standard) + ")\n";
                output += "set(CMAKE_CXX_EXTENSIONS OFF)\n";
            }
            if (options.msvc_runtime != MsvcRuntime::default_runtime || options.cxx_standard)
                output += '\n';

            output += "if(PROJECT_IS_TOP_LEVEL AND NOT KAIXA_CMAKE_DEPENDENCIES_INCLUDED)\n"
                      "    include(\"${CMAKE_CURRENT_LIST_DIR}/KaixaDependencies.cmake\")\n"
                      "endif()\n\n";
        }

        void generate_output_policy(std::string& output, const Options& options) {
            if (!options.runtime_output && !options.library_output && !options.archive_output)
                return;

            output += "if(PROJECT_IS_TOP_LEVEL)\n"
                      "    if(KAIXA_OUTPUT_ROOT)\n"
                      "        set(_kaixa_output_root \"${KAIXA_OUTPUT_ROOT}\")\n"
                      "    else()\n"
                      "        set(_kaixa_output_root \"${CMAKE_BINARY_DIR}\")\n"
                      "    endif()\n";
            if (options.runtime_output)
                output += "    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY " + output_directory(*options.runtime_output) + ")\n";
            if (options.library_output)
                output += "    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY " + output_directory(*options.library_output) + ")\n";
            if (options.archive_output)
                output += "    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY " + output_directory(*options.archive_output) + ")\n";

            output += "endif()\n\n";
        }

        void generate_target_declaration(std::string& output, const Options& options, const TargetOptions& target) {
            switch (target.type) {
            case TargetType::executable: output += "add_executable(" + target.name; break;
            case TargetType::static_library: output += "add_library(" + target.name + " STATIC"; break;
            case TargetType::shared_library: output += "add_library(" + target.name + " SHARED"; break;
            case TargetType::interface_library: output += "add_library(" + target.name + " INTERFACE"; break;
            }
            const std::vector<std::string> sources = project_paths(options, target.sources);
            if (sources.empty()) {
                output += ")\n\n";
                return;
            }

            output += "\n";
            for (const std::string& source: sources)
                output += "    " + syntax::argument(source) + "\n";

            output += ")\n\n";
        }

        void generate_target_usage(std::string& output, const Options& options, const TargetOptions& target) {
            const bool interface_target = target.type == TargetType::interface_library;
            const std::string_view private_scope = interface_target ? "INTERFACE" : "PRIVATE";
            const std::string_view public_scope = interface_target ? "INTERFACE" : "PUBLIC";
            emit_values(
                output,
                "target_include_directories",
                target.name,
                private_scope,
                project_paths(options, target.include_directories)
            );
            emit_values(output, "target_include_directories", target.name, public_scope, generated_public_includes(options, target));
            emit_values(
                output,
                "target_include_directories",
                target.name,
                interface_target ? "SYSTEM INTERFACE" : "SYSTEM PRIVATE",
                project_paths(options, target.system_include_directories)
            );
            emit_values(
                output,
                "target_include_directories",
                target.name,
                interface_target ? "SYSTEM INTERFACE" : "SYSTEM PUBLIC",
                generated_public_system_includes(options, target)
            );
            emit_values(output, "target_link_libraries", target.name, private_scope, project_link_values(options, target.link_libraries));
            emit_values(
                output,
                "target_link_libraries",
                target.name,
                public_scope,
                project_link_values(options, target.public_link_libraries)
            );
            emit_values(
                output,
                "target_compile_definitions",
                target.name,
                private_scope,
                project_definitions(options, target.compile_definitions)
            );
            emit_values(
                output,
                "target_compile_definitions",
                target.name,
                public_scope,
                project_definitions(options, target.public_compile_definitions)
            );
            emit_values(output, "target_compile_options", target.name, private_scope, target.compile_options);
            emit_values(output, "target_compile_options", target.name, public_scope, target.public_compile_options);
            emit_values(output, "target_link_options", target.name, private_scope, target.link_options);
            emit_values(
                output,
                "target_precompile_headers",
                target.name,
                private_scope,
                project_paths(options, target.precompiled_headers)
            );
        }

        void generate_target_policy(std::string& output, const TargetOptions& target) {
            const bool interface_target = target.type == TargetType::interface_library;
            if (target.msvc_runtime != MsvcRuntime::default_runtime) {
                const std::string runtime = target.msvc_runtime == MsvcRuntime::static_runtime ? "MultiThreaded" : "MultiThreadedDLL";
                output += "set_property(TARGET "
                    + target.name
                    + " PROPERTY MSVC_RUNTIME_LIBRARY \""
                    + runtime
                    + "$<$<CONFIG:Debug>:Debug>\")\n\n";
            }
            if (target.cxx_standard) {
                const std::string scope = interface_target ? "INTERFACE" : (target.type == TargetType::executable ? "PRIVATE" : "PUBLIC");
                output += "target_compile_features("
                    + target.name
                    + " "
                    + scope
                    + " cxx_std_"
                    + std::to_string(*target.cxx_standard)
                    + ")\n\n";
                output += "set_target_properties(" + target.name + " PROPERTIES CXX_EXTENSIONS OFF)\n\n";
            }
            if (!target.default_build)
                output += "set_target_properties(" + target.name + " PROPERTIES EXCLUDE_FROM_ALL TRUE)\n\n";
        }

        void generate_package(std::string& output, const ProjectPackage& project_package) {
            const PackageNode& package = *project_package.package;
            const Options& options = *project_package.options;
            generate_output_policy(output, options);
            if (options.msvc_runtime != MsvcRuntime::default_runtime) {
                const std::string runtime = options.msvc_runtime == MsvcRuntime::static_runtime ? "MultiThreaded" : "MultiThreadedDLL";
                output += "set(CMAKE_MSVC_RUNTIME_LIBRARY \"" + runtime + "$<$<CONFIG:Debug>:Debug>\")\n\n";
            }
            for (const std::string& package_name: options.find_packages)
                output += "find_package(" + package_name + " REQUIRED)\n";
            if (!options.find_packages.empty())
                output += "\n";

            for (const TargetOptions& target: options.targets) {
                generate_target_declaration(output, options, target);
                generate_target_usage(output, options, target);
                generate_target_policy(output, target);
                generate_runtime_materialization(output, options, target);
                generate_target_install(output, options, package.name, target);
            }
            generate_package_export(output, package.name, project_version(package), options.targets, options.export_dependencies);
            generate_tests(output, options.tests);
        }
    }

    std::string generate_project(const std::span<const ProjectPackage> packages) {
        if (packages.empty())
            return {};

        std::string output;
        generate_project_header(output, packages);
        generate_export_bootstrap(output, *packages.front().options);
        for (const ProjectPackage& package: packages)
            generate_package(output, package);

        return output;
    }
}
