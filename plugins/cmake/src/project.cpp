#include "configuration.hpp"
#include "testing.hpp"

#include <algorithm>
#include <filesystem>
#include <ranges>
#include <string>
#include <vector>

namespace kaixa::plugin::cmake::detail {
    namespace {
        std::string quote(const std::string_view value) {
            std::string equals;
            while (value.contains("]" + equals + "]"))
                equals += '=';

            return "[" + equals + "[" + std::string(value) + "]" + equals + "]";
        }

        std::string quoted_string(const std::string_view value) {
            std::string result = "\"";
            for (const char character: value) {
                if (character == '\\' || character == '"')
                    result.push_back('\\');

                result.push_back(character);
            }
            result.push_back('"');
            return result;
        }

        std::string cmake_argument(const std::string_view value) {
            return value.contains("${") ? quoted_string(value) : quote(value);
        }

        std::string project_path(const Options& options, const std::string_view value) {
            if (value.starts_with("$<") || value.starts_with('<'))
                return std::string(value);

            const std::filesystem::path path(value);
            if (options.generation == GenerationMode::state)
                return (path.is_absolute() ? path : options.source / path).lexically_normal().generic_string();

            if (path.is_relative())
                return path.lexically_normal().generic_string();

            const std::filesystem::path relative = path.lexically_relative(options.source);
            return relative.empty() ? path.generic_string() : relative.generic_string();
        }

        std::string project_source_path(const Options& options, const std::filesystem::path& path) {
            std::string value = project_path(options, path.generic_string());
            if (options.generation == GenerationMode::source && std::filesystem::path(value).is_relative())
                return "${CMAKE_CURRENT_LIST_DIR}/" + value;

            return value;
        }

        std::string output_directory(const std::filesystem::path& path) {
            const std::string value = path.generic_string();
            return path.is_absolute() ? quoted_string(value) : quoted_string("${_kaixa_output_root}/" + value + "/$<0:>");
        }

        std::string project_version(const PackageNode& package) {
            if (!package.manifest || !package.manifest->version)
                return {};

            std::string value = package.manifest->version->text;
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
                output += "    " + cmake_argument(value) + "\n";

            output += ")\n\n";
        }

        std::vector<std::string> project_paths(const Options& options, const std::vector<std::string>& values) {
            if (options.generation == GenerationMode::source)
                return values;

            std::vector<std::string> result;
            result.reserve(values.size());
            for (const std::string& value: values)
                result.push_back(project_path(options, value));

            return result;
        }

        void generate_runtime_materialization(std::string& output, const Options& options, const TargetOptions& target) {
            for (const TargetOptions::RuntimeFile& runtime_file: target.runtime_files) {
                const std::string destination = "$<TARGET_FILE_DIR:" + target.name + ">/" + runtime_file.destination.generic_string();
                const std::filesystem::path parent = runtime_file.destination.parent_path();
                output += "add_custom_command(TARGET " + target.name + " POST_BUILD\n";
                output += "    COMMAND ${CMAKE_COMMAND} -E make_directory "
                    + quote("$<TARGET_FILE_DIR:" + target.name + ">/" + parent.generic_string())
                    + "\n";
                output += "    COMMAND ${CMAKE_COMMAND} -E "
                    + std::string(runtime_file.directory ? "copy_directory " : "copy_if_different ")
                    + cmake_argument(project_source_path(options, runtime_file.source))
                    + " "
                    + quote(destination)
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
                if (options.generation == GenerationMode::source && !include.starts_with("$<") && path.is_relative())
                    include.insert(0, "${CMAKE_CURRENT_LIST_DIR}/");

                include.insert(0, "$<BUILD_INTERFACE:");
                include += '>';
            }
            result.push_back("$<INSTALL_INTERFACE:include>");
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
                    + cmake_argument(project_source_path(options, header.source))
                    + " DESTINATION "
                    + quote((std::filesystem::path("include") / header.destination.parent_path()).generic_string());
                if (header.source.filename() != header.destination.filename())
                    output += " RENAME " + quote(header.destination.filename().generic_string());

                output += ")\n";
            }
            if (!target.install_headers.empty())
                output += '\n';

            for (const TargetOptions::RuntimeFile& runtime_file: target.runtime_files) {
                const std::filesystem::path destination = std::filesystem::path("bin") / runtime_file.destination.parent_path();
                if (runtime_file.directory) {
                    output += "install(DIRECTORY "
                        + cmake_argument(project_source_path(options, runtime_file.source) + "/")
                        + " DESTINATION "
                        + quote((destination / runtime_file.destination.filename()).generic_string())
                        + ")\n";
                } else {
                    output += "install(FILES "
                        + cmake_argument(project_source_path(options, runtime_file.source))
                        + " DESTINATION "
                        + quote(destination.generic_string());
                    if (runtime_file.source.filename() != runtime_file.destination.filename())
                        output += " RENAME " + quote(runtime_file.destination.filename().generic_string());

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
            const std::span<const TargetOptions> targets
        ) {
            if (std::ranges::none_of(targets, &TargetOptions::install))
                return;

            const std::string name(package);
            const std::string config = name + "Config.cmake";
            output += "file(WRITE \"${CMAKE_CURRENT_BINARY_DIR}/"
                + config
                + "\" \"include(\\\"\\${CMAKE_CURRENT_LIST_DIR}/"
                + name
                + "Targets.cmake\\\")\\n\")\n";
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
    }

    std::string generate_project(const PackageNode& package, const Options& options) {
        const std::string version = project_version(package);
        std::string output = std::string(generated_marker) + "\n" + "cmake_minimum_required(VERSION 3.20)\n" + "project(" + package.name;
        if (!version.empty())
            output += " VERSION " + version;

        output += " LANGUAGES";
        for (const std::string& language: options.languages)
            output += " " + language;

        output += ")\n\n";

        if (options.runtime_output || options.library_output || options.archive_output) {
            output += "if(PROJECT_IS_TOP_LEVEL)\n";
            output += "    if(KAIXA_OUTPUT_ROOT)\n";
            output += "        set(_kaixa_output_root \"${KAIXA_OUTPUT_ROOT}\")\n";
            output += "    else()\n";
            output += "        set(_kaixa_output_root \"${CMAKE_BINARY_DIR}\")\n";
            output += "    endif()\n";
            if (options.runtime_output)
                output += "    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY " + output_directory(*options.runtime_output) + ")\n";

            if (options.library_output)
                output += "    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY " + output_directory(*options.library_output) + ")\n";

            if (options.archive_output)
                output += "    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY " + output_directory(*options.archive_output) + ")\n";

            output += "endif()\n\n";
        }

        if (options.msvc_runtime != MsvcRuntime::default_runtime) {
            const std::string runtime = options.msvc_runtime == MsvcRuntime::static_runtime ? "MultiThreaded" : "MultiThreadedDLL";
            output += "set(CMAKE_MSVC_RUNTIME_LIBRARY \"" + runtime + "$<$<CONFIG:Debug>:Debug>\")\n\n";
        }

        for (const std::string& package_name: options.find_packages)
            output += "find_package(" + package_name + " REQUIRED)\n";
        if (!options.find_packages.empty())
            output += "\n";

        for (const TargetOptions& target: options.targets) {
            switch (target.type) {
            case TargetType::executable: output += "add_executable(" + target.name; break;
            case TargetType::static_library: output += "add_library(" + target.name + " STATIC"; break;
            case TargetType::shared_library: output += "add_library(" + target.name + " SHARED"; break;
            case TargetType::interface_library: output += "add_library(" + target.name + " INTERFACE"; break;
            }
            const std::vector<std::string> sources = project_paths(options, target.sources);
            if (sources.empty()) {
                output += ")\n\n";
            } else {
                output += "\n";
                for (const std::string& source: sources)
                    output += "    " + quote(source) + "\n";

                output += ")\n\n";
            }

            const bool interface_target = target.type == TargetType::interface_library;
            const bool executable = target.type == TargetType::executable;
            emit_values(
                output,
                "target_include_directories",
                target.name,
                interface_target ? "INTERFACE" : "PRIVATE",
                project_paths(options, target.include_directories)
            );
            emit_values(
                output,
                "target_include_directories",
                target.name,
                interface_target ? "INTERFACE" : "PUBLIC",
                generated_public_includes(options, target)
            );
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
                project_paths(options, target.public_system_include_directories)
            );
            emit_values(output, "target_link_libraries", target.name, interface_target ? "INTERFACE" : "PRIVATE", target.link_libraries);
            emit_values(
                output,
                "target_link_libraries",
                target.name,
                interface_target ? "INTERFACE" : "PUBLIC",
                target.public_link_libraries
            );
            emit_values(
                output,
                "target_compile_definitions",
                target.name,
                interface_target ? "INTERFACE" : "PRIVATE",
                target.compile_definitions
            );
            emit_values(
                output,
                "target_compile_definitions",
                target.name,
                interface_target ? "INTERFACE" : "PUBLIC",
                target.public_compile_definitions
            );
            emit_values(output, "target_compile_options", target.name, interface_target ? "INTERFACE" : "PRIVATE", target.compile_options);
            emit_values(
                output,
                "target_compile_options",
                target.name,
                interface_target ? "INTERFACE" : "PUBLIC",
                target.public_compile_options
            );
            emit_values(output, "target_link_options", target.name, interface_target ? "INTERFACE" : "PRIVATE", target.link_options);
            emit_values(
                output,
                "target_precompile_headers",
                target.name,
                interface_target ? "INTERFACE" : "PRIVATE",
                project_paths(options, target.precompiled_headers)
            );

            if (target.msvc_runtime != MsvcRuntime::default_runtime) {
                const std::string runtime = target.msvc_runtime == MsvcRuntime::static_runtime ? "MultiThreaded" : "MultiThreadedDLL";
                output += "set_property(TARGET "
                    + target.name
                    + " PROPERTY MSVC_RUNTIME_LIBRARY \""
                    + runtime
                    + "$<$<CONFIG:Debug>:Debug>\")\n\n";
            }

            if (target.cxx_standard) {
                const std::string scope = interface_target ? "INTERFACE" : (executable ? "PRIVATE" : "PUBLIC");
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

            generate_runtime_materialization(output, options, target);
            generate_target_install(output, options, package.name, target);
        }

        generate_package_export(output, package.name, version, options.targets);
        generate_tests(output, options.tests);
        return output;
    }
}
