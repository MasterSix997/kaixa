#pragma once

#include <kaixa/foundation/diagnostic.hpp>
#include <kaixa/model/manifest.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kaixa {
    enum class TestAdapterPurpose {
        test,
        benchmark
    };

    struct TestAdapterInfo {
        std::string name;
        TestAdapterPurpose purpose = TestAdapterPurpose::test;
        std::string dependency;
        std::string main_product;
        std::vector<std::string> discovery_arguments;
        std::string case_filter_prefix;
        std::string case_filter_suffix;
    };

    [[nodiscard]] Result<TestAdapterInfo> test_adapter(
        std::string_view framework,
        PackageTargetKind kind,
        const SourceLocation& location = {}
    );
    [[nodiscard]] Result<std::vector<std::string>> parse_test_cases(const TestAdapterInfo& adapter, std::string_view output);
    [[nodiscard]] std::vector<std::string> test_case_arguments(
        const TestAdapterInfo& adapter,
        std::string_view test_case,
        std::span<const std::string> arguments = {}
    );
}
