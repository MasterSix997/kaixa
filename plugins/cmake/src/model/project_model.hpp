#pragma once

#include <kaixa/model/graph.hpp>
#include <kaixa/test/adapter.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kaixa::plugin::cmake::detail {
    // The resolver's own typed model of a CMake project. Schema reading produces it, generation
    // consumes it, and nothing here is a `Value`: manifest text is interpreted exactly once,
    // before this model exists.
    inline constexpr std::string_view test_target_label_prefix = "kaixa.target:";

    enum class DependencyMode {
        add_subdirectory,
        find_package
    };

    enum class TargetType {
        executable,
        static_library,
        shared_library,
        interface_library
    };

    enum class GenerationMode {
        export_project,
        state
    };

    enum class MsvcRuntime {
        default_runtime,
        static_runtime,
        dynamic_runtime
    };

    struct DependencyOption {
        PackageId package;
        DependencyMode mode = DependencyMode::add_subdirectory;
    };

    struct TargetOptions {
        struct RuntimeFile {
            std::filesystem::path source;
            std::filesystem::path destination;
            bool directory = false;
        };

        struct InstallHeader {
            std::filesystem::path source;
            std::filesystem::path destination;
        };

        std::string name;
        TargetType type = TargetType::executable;
        std::vector<std::string> sources;
        std::vector<std::string> include_directories;
        std::vector<std::string> public_include_directories;
        std::vector<std::string> system_include_directories;
        std::vector<std::string> public_system_include_directories;
        std::vector<std::string> link_libraries;
        std::vector<std::string> public_link_libraries;
        std::vector<std::string> compile_definitions;
        std::vector<std::string> public_compile_definitions;
        std::vector<std::string> compile_options;
        std::vector<std::string> public_compile_options;
        std::vector<std::string> link_options;
        std::vector<std::string> precompiled_headers;
        std::vector<RuntimeFile> runtime_files;
        std::vector<InstallHeader> install_headers;
        std::optional<std::int64_t> cxx_standard;
        MsvcRuntime msvc_runtime = MsvcRuntime::default_runtime;
        bool default_build = true;
        bool install = false;
    };

    struct TestOptions {
        std::string name;
        std::string target;
        std::vector<std::string> arguments;
        TestAdapterInfo adapter;
    };

    struct PortableSourceRoot {
        std::filesystem::path directory;
        std::string variable;
    };

    struct Options {
        std::filesystem::path source;
        std::vector<std::string> languages;
        GenerationMode generation = GenerationMode::export_project;
        MsvcRuntime msvc_runtime = MsvcRuntime::default_runtime;
        std::optional<std::int64_t> cxx_standard;
        std::optional<std::filesystem::path> runtime_output;
        std::optional<std::filesystem::path> library_output;
        std::optional<std::filesystem::path> archive_output;
        std::vector<TargetOptions> targets;
        std::vector<TestOptions> tests;
        std::vector<DependencyOption> dependencies;
        std::vector<std::string> find_packages;
        std::vector<std::string> export_dependencies;
        std::vector<PortableSourceRoot> portable_source_roots;
        std::string policy_fingerprint;
    };

    struct BuildOptions {
        std::optional<std::string> generator;
        std::optional<std::string> c_compiler;
        std::optional<std::string> cxx_compiler;
        std::optional<std::filesystem::path> toolchain;
        std::vector<std::string> configure_arguments;
        std::vector<std::string> build_arguments;
        std::vector<std::string> install_arguments;
    };

    [[nodiscard]] DependencyMode dependency_mode(const Options& options, PackageId dependency);
}
