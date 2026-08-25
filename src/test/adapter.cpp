#include <kaixa/test/adapter.hpp>

#include <kaixa/extension/registry.hpp>
#include <kaixa/model/manifest.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

namespace kaixa {
    namespace {
        std::string_view trim(const std::string_view value) {
            const auto whitespace = [](const unsigned char character) { return std::isspace(character) != 0; };
            const auto first = std::ranges::find_if_not(value, whitespace);
            if (first == value.end())
                return {};

            const auto last = std::ranges::find_if_not(value.rbegin(), value.rend(), whitespace).base();
            return {first, last};
        }

        std::vector<std::string_view> lines(const std::string_view output) {
            std::vector<std::string_view> result;
            std::size_t offset = 0;
            while (offset <= output.size()) {
                const std::size_t end = output.find('\n', offset);
                std::string_view line = end == std::string_view::npos ? output.substr(offset) : output.substr(offset, end - offset);
                if (!line.empty() && line.back() == '\r')
                    line.remove_suffix(1);

                result.push_back(line);
                if (end == std::string_view::npos)
                    break;

                offset = end + 1;
            }
            return result;
        }
    }

    void add_standard_test_adapters(ExtensionRegistry& registry) {
        registry.add(TestAdapterInfo{"executable", TestAdapterPurpose::test});
        registry.add(TestAdapterInfo{"executable-benchmark", TestAdapterPurpose::benchmark});
        registry.add(
            TestAdapterInfo{"kaixa",
                TestAdapterPurpose::test,
                {},
                {},
                {"--kaixa-test-list"},
                "--kaixa-test-run",
                {},
                TestCaseListingFormat::lines,
                true}
        );
        registry.add(
            TestAdapterInfo{"googletest",
                TestAdapterPurpose::test,
                "googletest",
                "googletest",
                {"--gtest_list_tests"},
                "--gtest_filter=",
                {},
                TestCaseListingFormat::googletest}
        );
        registry.add(
            TestAdapterInfo{"google-benchmark",
                TestAdapterPurpose::benchmark,
                "google_benchmark",
                "google_benchmark",
                {"--benchmark_list_tests=true"},
                "--benchmark_filter=^",
                "$"}
        );
    }

    std::string_view default_test_adapter(const PackageTargetKind kind, const bool discover) {
        if (discover)
            return "kaixa";

        return kind == PackageTargetKind::benchmark ? "executable-benchmark" : "executable";
    }

    Result<TestAdapterInfo> test_adapter(
        const ExtensionRegistry& registry,
        const std::string_view framework,
        const PackageTargetKind kind,
        const SourceLocation& location
    ) {
        const TestAdapterInfo* adapter = registry.find_test_adapter(framework);
        if (!adapter)
            return std::unexpected(error_at(location, "test adapter `" + std::string(framework) + "` is not installed"));

        const TestAdapterPurpose expected = kind == PackageTargetKind::benchmark ? TestAdapterPurpose::benchmark : TestAdapterPurpose::test;
        if (adapter->purpose != expected) {
            return std::unexpected(error_at(
                location,
                "test adapter `"
                    + std::string(framework)
                    + "` does not support "
                    + (kind == PackageTargetKind::benchmark ? "benchmark" : "test")
                    + " targets"
            ));
        }
        return *adapter;
    }

    Result<std::vector<std::string>> parse_test_cases(const TestAdapterInfo& adapter, const std::string_view output) {
        std::vector<std::string> result;
        if (adapter.discovery_arguments.empty())
            return result;

        if (adapter.listing_format == TestCaseListingFormat::googletest) {
            std::string suite;
            for (const std::string_view untrimmed: lines(output)) {
                const std::string_view line = trim(untrimmed);
                if (line.empty())
                    continue;

                const std::string_view value = line.substr(0, line.find("  #"));
                if (!untrimmed.empty() && std::isspace(static_cast<unsigned char>(untrimmed.front())) != 0) {
                    if (!suite.empty())
                        result.push_back(suite + std::string(value));
                } else {
                    suite = value;
                }
            }
        } else {
            for (const std::string_view untrimmed: lines(output)) {
                const std::string_view line = trim(untrimmed);
                if (!line.empty())
                    result.emplace_back(line);
            }
        }
        std::ranges::sort(result);
        result.erase(std::ranges::unique(result).begin(), result.end());
        return result;
    }

    std::vector<std::string> test_case_arguments(
        const TestAdapterInfo& adapter,
        const std::string_view test_case,
        const std::span<const std::string> arguments
    ) {
        std::vector<std::string> result(arguments.begin(), arguments.end());
        if (adapter.separate_filter_argument) {
            result.push_back(adapter.case_filter_prefix);
            result.emplace_back(test_case);
        } else if (!adapter.case_filter_prefix.empty()) {
            result.push_back(adapter.case_filter_prefix + std::string(test_case) + adapter.case_filter_suffix);
        }

        return result;
    }
}
