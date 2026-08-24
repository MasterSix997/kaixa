#pragma once

#include <kaixa/foundation/diagnostic.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kaixa {
    class ExtensionRegistry;
    enum class PackageTargetKind;

    enum class TestAdapterPurpose {
        test,
        benchmark
    };

    enum class TestCaseListingFormat {
        lines,
        googletest
    };

    struct TestAdapterInfo {
        std::string name;
        TestAdapterPurpose purpose = TestAdapterPurpose::test;
        std::string dependency;
        std::string main_product;
        std::vector<std::string> discovery_arguments;
        std::string case_filter_prefix;
        std::string case_filter_suffix;
        TestCaseListingFormat listing_format = TestCaseListingFormat::lines;
        bool separate_filter_argument = false;
    };

    void add_standard_test_adapters(ExtensionRegistry& registry);

    [[nodiscard]] std::string_view default_test_adapter(PackageTargetKind kind, bool discover);

    [[nodiscard]] Result<TestAdapterInfo> test_adapter(
        const ExtensionRegistry& registry,
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
