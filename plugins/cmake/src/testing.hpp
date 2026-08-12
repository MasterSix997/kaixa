#pragma once

#include "configuration.hpp"

#include <kaixa/build/plan.hpp>
#include <kaixa/extension/resolver.hpp>
#include <kaixa/foundation/diagnostic.hpp>
#include <kaixa/model/package.hpp>

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace kaixa::plugin::cmake::detail {
    void generate_tests(std::string& output, std::span<const TestOptions> tests);

    [[nodiscard]] Result<void> plan_tests(
        const Options& options,
        const PackageNode& package,
        const std::filesystem::path& build_directory,
        std::string_view configuration,
        const TestRequest& request,
        BuildPlan& plan
    );
}
