#pragma once

#include <model/project_model.hpp>

#include <kaixa/build/layout.hpp>
#include <kaixa/model/package.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kaixa::plugin::cmake::detail {
    // The identity of one configured build tree. Planning derives it from the environment and the
    // typed project model, and everything downstream addresses build trees through it.
    struct BuildVariant {
        std::string label;
        std::string fingerprint;
        std::string directory;
    };

    [[nodiscard]] std::optional<std::string> requested_generator(const std::vector<std::string>& arguments);
    [[nodiscard]] bool uses_multiple_configurations(const std::optional<std::string>& requested);
    [[nodiscard]] BuildVariant build_variant(
        const BuildEnvironment& environment,
        const BuildOptions& options,
        const std::vector<std::string>& arguments,
        const Options& project,
        const ConfiguredPackageInstance& instance,
        bool primary
    );
    [[nodiscard]] std::filesystem::path cmake_build_root(const BuildEnvironment& environment, std::string_view variant);
    [[nodiscard]] std::filesystem::path artifact_directory(const BuildEnvironment& environment, const ConfiguredPackageInstance& instance);
}
