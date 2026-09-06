#pragma once

#include <model/project_model.hpp>

#include <kaixa/build/plan.hpp>
#include <kaixa/extension/resolver.hpp>
#include <kaixa/foundation/diagnostic.hpp>
#include <kaixa/model/package.hpp>

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace kaixa::plugin::cmake::detail {
    struct TestPlanRoute {
        std::filesystem::path build_directory;
        std::string_view configuration;
        std::span<const std::string> selected_targets;
        std::string_view configured_artifact;
    };

    void generate_tests(std::string& output, std::span<const TestOptions> tests);

    [[nodiscard]] Result<void> plan_tests(
        const Options& options,
        const PackageNode& package,
        const TestPlanRoute& route,
        const TestRequest& request,
        ExecutionPlan& plan
    );
}
