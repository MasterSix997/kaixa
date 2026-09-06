#include "testing.hpp"
#include <generation/cmake_syntax.hpp>

#include <algorithm>
#include <ranges>
#include <utility>

namespace kaixa::plugin::cmake::detail {
    namespace {
        std::string regex_escape(const std::string_view value) {
            constexpr std::string_view special = R"(\.^$|()[]*+?{})";
            std::string escaped;
            escaped.reserve(value.size());
            for (const char character: value) {
                if (special.contains(character))
                    escaped += '\\';

                escaped += character;
            }
            return escaped;
        }

        std::string test_labels(const TestOptions& test) {
            return std::string(test_target_label_prefix)
                + test.target
                + ";kaixa.purpose:"
                + (test.adapter.purpose == TestAdapterPurpose::benchmark ? "benchmark" : "test");
        }

        std::string discovery_script(const TestOptions& test) {
            const std::string label = test_labels(test);
            std::string output;
            output += "set(_kaixa_test_executable " + syntax::literal("$<TARGET_FILE:" + test.target + ">") + ")\n";
            output += "set(_kaixa_test_prefix " + syntax::literal(test.name) + ")\n";
            output += "set(_kaixa_test_label " + syntax::literal(label) + ")\n";
            output += "if(NOT EXISTS \"${_kaixa_test_executable}\")\n  return()\nendif()\n";
            output += "execute_process(\n  COMMAND \"${_kaixa_test_executable}\"";
            for (const std::string& argument: test.adapter.discovery_arguments)
                output += " " + syntax::literal(argument);

            output += R"cmake(
  RESULT_VARIABLE _kaixa_result
  OUTPUT_VARIABLE _kaixa_output
  ERROR_VARIABLE _kaixa_error
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT _kaixa_result EQUAL 0)
  message(FATAL_ERROR "test discovery failed: ${_kaixa_error}")

endif()
string(REPLACE "\n" ";" _kaixa_cases "${_kaixa_output}")
foreach(_kaixa_case IN LISTS _kaixa_cases)
  if(_kaixa_case STREQUAL "")
    continue()

  endif()
  set(_kaixa_name "${_kaixa_test_prefix}::${_kaixa_case}")
  add_test("${_kaixa_name}" "${_kaixa_test_executable}")cmake";
            if (test.adapter.separate_filter_argument) {
                output += " " + syntax::literal(test.adapter.case_filter_prefix) + " \"${_kaixa_case}\"";
            } else if (!test.adapter.case_filter_prefix.empty()) {
                output += " " + syntax::literal(test.adapter.case_filter_prefix + "${_kaixa_case}" + test.adapter.case_filter_suffix);
            }
            for (const std::string& argument: test.arguments)
                output += " " + syntax::literal(argument);

            output += R"cmake()
  set_tests_properties("${_kaixa_name}" PROPERTIES LABELS "${_kaixa_test_label}")
endforeach()
)cmake";
            return output;
        }

        void generate_discovered_test(std::string& output, const TestOptions& test, const std::size_t index) {
            const std::string variable = "_kaixa_discovery_" + std::to_string(index);
            const std::string filename = "kaixa-discovery-" + std::to_string(index);
            const std::string script = discovery_script(test);

            output += "set(" + variable + " \"${CMAKE_CURRENT_BINARY_DIR}/" + filename + "\")\n";
            output += "if(CMAKE_CONFIGURATION_TYPES)\n";
            output += "  file(GENERATE OUTPUT \"${" + variable + "}-$<CONFIG>.cmake\" CONTENT " + syntax::literal(script) + ")\n";
            output += "  file(WRITE \"${"
                + variable
                + "}.cmake\""
                + " \"include(\\\"${"
                + variable
                + "}-\\${CTEST_CONFIGURATION_TYPE}.cmake\\\")\\n\")\n";
            output += "else()\n";
            output += "  file(GENERATE OUTPUT \"${" + variable + "}.cmake\" CONTENT " + syntax::literal(script) + ")\n";
            output += "endif()\n";
            output += "set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES";
            output += " \"${" + variable + "}.cmake\")\n";
        }

        void generate_googletest(std::string& output, const TestOptions& test) {
            output += "include(GoogleTest)\n";
            output += "gtest_discover_tests("
                + test.target
                + " TEST_PREFIX "
                + syntax::literal(test.name + "::")
                + " DISCOVERY_MODE PRE_TEST";
            if (!test.arguments.empty()) {
                output += " EXTRA_ARGS";
                for (const std::string& argument: test.arguments)
                    output += " " + syntax::literal(argument);
            }
            std::string labels = test_labels(test);
            labels.replace(labels.find(';'), 1, "\\;");
            output += " PROPERTIES LABELS \"" + labels + "\")\n";
        }

        Result<Action*> find_build_action(ExecutionPlan& plan, const PackageNode& package, const std::string_view configured_artifact) {
            const std::span<Action> builds = plan.builds();
            const auto action = std::ranges::find_if(builds, [&](const Action& candidate) {
                return candidate.package == package.id && candidate.configured_artifact == configured_artifact;
            });
            if (action == builds.end()) {
                return std::unexpected(error("CMake test plan has no build action for package `" + package.name + "`"));
            }

            return &*action;
        }
    }

    void generate_tests(std::string& output, const std::span<const TestOptions> tests) {
        if (tests.empty())
            return;

        output += "enable_testing()\n\n";
        for (std::size_t index = 0; index < tests.size(); ++index) {
            const TestOptions& test = tests[index];
            if (test.adapter.listing_format == TestCaseListingFormat::googletest) {
                generate_googletest(output, test);
                continue;
            }
            if (!test.adapter.discovery_arguments.empty()) {
                generate_discovered_test(output, test, index);
                continue;
            }

            output += "add_test(NAME " + syntax::literal(test.name) + " COMMAND " + test.target;
            for (const std::string& argument: test.arguments)
                output += " " + syntax::literal(argument);

            output += ")\n";
            output += "set_tests_properties("
                + syntax::literal(test.name)
                + " PROPERTIES LABELS "
                + syntax::literal(test_labels(test))
                + ")\n";
        }
    }

    Result<void> plan_tests(
        const Options& options,
        const PackageNode& package,
        const TestPlanRoute& route,
        const TestRequest& request,
        ExecutionPlan& plan
    ) {
        std::vector<std::string> build_targets;
        if (!route.selected_targets.empty()) {
            build_targets.assign(route.selected_targets.begin(), route.selected_targets.end());
            for (const std::string& selected: build_targets) {
                if (std::ranges::none_of(options.tests, [&](const TestOptions& test) {
                        return test.target == selected
                            && (request.purpose == ProductPurpose::benchmark) == (test.adapter.purpose == TestAdapterPurpose::benchmark);
                    })) {
                    return std::unexpected(error("CMake target `" + selected + "` does not declare tests"));
                }
            }
        } else if (request.target) {
            const auto target = std::ranges::find_if(options.targets, [&](const TargetOptions& candidate) {
                return candidate.name == *request.target;
            });
            if (target == options.targets.end()) {
                return std::unexpected(error("CMake target `" + *request.target + "` does not exist"));
            }
            if (std::ranges::none_of(options.tests, [&](const TestOptions& test) {
                    return test.target == *request.target
                        && (request.purpose == ProductPurpose::benchmark) == (test.adapter.purpose == TestAdapterPurpose::benchmark);
                })) {
                return std::unexpected(error("CMake target `" + *request.target + "` does not declare tests"));
            }

            build_targets.push_back(*request.target);
        } else {
            for (const TestOptions& test: options.tests) {
                if ((request.purpose == ProductPurpose::benchmark) != (test.adapter.purpose == TestAdapterPurpose::benchmark))
                    continue;

                if (std::ranges::find(build_targets, test.target) == build_targets.end())
                    build_targets.push_back(test.target);
            }
        }

        if (!build_targets.empty()) {
            auto build = find_build_action(plan, package, route.configured_artifact);
            if (!build)
                return std::unexpected(build.error());

            (*build)->argv.push_back("--target");
            (*build)->argv.insert((*build)->argv.end(), build_targets.begin(), build_targets.end());
        }

        const bool list = request.mode == TestMode::list;
        Action action;
        action.description = list ? "list tests " + package.name : "test " + package.name;
        action.argv = {"ctest", "--test-dir", route.build_directory.string(), "--build-config", std::string(route.configuration)};
        action.argv.push_back(list ? "--show-only" : "--output-on-failure");
        if (request.filter) {
            action.argv.push_back("--tests-regex");
            action.argv.push_back(regex_escape(*request.filter));
        }
        if (!build_targets.empty()) {
            std::string labels = "^";
            if (build_targets.size() == 1) {
                labels += regex_escape(test_target_label_prefix) + regex_escape(build_targets.front());
            } else {
                labels += '(';
                for (std::size_t index = 0; index < build_targets.size(); ++index) {
                    if (index != 0)
                        labels += '|';

                    labels += regex_escape(test_target_label_prefix) + regex_escape(build_targets[index]);
                }
                labels += ')';
            }
            labels += '$';
            action.argv.push_back("--label-regex");
            action.argv.push_back(std::move(labels));
        } else {
            action.argv.push_back("--label-regex");
            action.argv.push_back(request.purpose == ProductPurpose::benchmark ? "^kaixa\\.purpose:benchmark$" : "^kaixa\\.purpose:test$");
        }
        action.working_directory = package.directory;
        action.package = package.id;
        action.configured_artifact = std::string(route.configured_artifact);
        plan.test(std::move(action));
        return {};
    }
}
