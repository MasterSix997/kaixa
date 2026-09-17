#include "variants.hpp"

#include <kaixa/foundation/process.hpp>

#include <array>
#include <charconv>
#include <cstdint>

namespace kaixa::plugin::cmake::detail {
    namespace {
        std::string variant_label(const EffectiveBuildConfiguration& configuration) {
            std::string label;
            const std::size_t visible = std::min(configuration.selected.size(), std::size_t{2});
            for (std::size_t index = 0; index < visible; ++index) {
                if (!label.empty())
                    label += '+';

                label += configuration.selected[index];
            }
            if (configuration.selected.size() > visible)
                label += '+' + std::to_string(configuration.selected.size() - visible);

            if (label.empty())
                label = configuration.profile;

            // A command-line profile is the effective build identity.  Keep the
            // resolver configuration name (it still supplies compiler/generator
            // settings), but do not put a stale profile such as `clang-debug` in
            // the output path when `--profile release` was requested.
            if (configuration.profile_origin.source == "command line" && !configuration.selected.empty()) {
                constexpr std::array<std::string_view, 4> known_profiles = {"debug", "release", "relwithdebinfo", "minsizerel"};
                bool replaced = false;
                for (const std::string_view known: known_profiles) {
                    const std::string suffix = "-" + std::string(known);
                    if (label.ends_with(suffix)) {
                        label.erase(label.size() - suffix.size());
                        label += "-" + configuration.profile;
                        replaced = true;
                        break;
                    }
                }
                if (!replaced)
                    label += "+" + configuration.profile;
            }

            for (char& character: label) {
                const bool valid = (character >= 'a' && character <= 'z')
                    || (character >= 'A' && character <= 'Z')
                    || (character >= '0' && character <= '9')
                    || character == '-'
                    || character == '_'
                    || character == '+';
                if (!valid)
                    character = '_';
            }
            if (label.size() > 48)
                label = label.substr(0, 45) + "...";

            return label;
        }
    }

    std::optional<std::string> requested_generator(const std::vector<std::string>& arguments) {
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            const std::string& argument = arguments[index];
            if ((argument == "-G" || argument == "--generator") && index + 1 < arguments.size()) {
                return arguments[index + 1];
            }
            if (argument.starts_with("-G") && argument.size() > 2)
                return argument.substr(2);

            if (argument.starts_with("--generator="))
                return argument.substr(std::string("--generator=").size());
        }
        return std::nullopt;
    }

    bool uses_multiple_configurations(const std::optional<std::string>& requested) {
        const std::optional<std::string> environment = environment_variable("CMAKE_GENERATOR");
        std::string_view generator;
        if (requested) {
            generator = *requested;
        } else if (environment) {
            generator = *environment;
        }

        if (!generator.empty()) {
            return generator.contains("Visual Studio") || generator.contains("Xcode") || generator.contains("Multi-Config");
        }

#ifdef _WIN32
        return true;
#else
        return false;
#endif
    }

    bool exports_compile_commands(const std::optional<std::string>& requested) {
        if (!uses_multiple_configurations(requested))
            return true;

        const std::optional<std::string> environment = environment_variable("CMAKE_GENERATOR");
        if (requested)
            return requested->contains("Ninja");

        return environment && environment->contains("Ninja");
    }

    BuildVariant build_variant(
        const BuildEnvironment& environment,
        const detail::BuildOptions& options,
        const std::vector<std::string>& arguments,
        const Options& project,
        const ConfiguredPackageInstance& instance,
        const bool primary
    ) {
        std::uint64_t hash = 14695981039346656037ull;
        const auto absorb = [&](const std::string_view value) {
            for (const char character: value) {
                const auto byte = static_cast<unsigned char>(character);
                hash ^= byte;
                hash *= 1099511628211ull;
            }
            hash ^= 0xffu;
            hash *= 1099511628211ull;
        };

        absorb(environment.configuration.profile);
        absorb(project.source.generic_string());
        absorb(project.generation == GenerationMode::export_project ? "export" : "state");
        absorb(project.policy_fingerprint);
        absorb(instance.artifact);
        if (options.generator)
            absorb(*options.generator);

        if (options.c_compiler)
            absorb(*options.c_compiler);

        if (options.cxx_compiler)
            absorb(*options.cxx_compiler);

        if (options.toolchain)
            absorb(options.toolchain->generic_string());

        for (const std::string& argument: arguments)
            absorb(argument);

        char encoded[16];
        const auto converted = std::to_chars(encoded, encoded + sizeof(encoded), hash, 16);
        const std::string fingerprint(encoded, converted.ptr);
        const std::string label = variant_label(environment.configuration);
        const std::string directory = primary ? label : label + "/instances/" + instance.artifact;
        return {label, fingerprint, directory};
    }

    std::filesystem::path cmake_build_root(const BuildEnvironment& environment, const std::string_view variant) {
        return environment.state_root / "build" / "cmake" / variant;
    }

    std::filesystem::path artifact_directory(const BuildEnvironment& environment, const ConfiguredPackageInstance& instance) {
        return environment.state_root / "cache" / "cmake" / instance.artifact;
    }
}
