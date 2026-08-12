#include "testing.hpp"

#include <algorithm>
#include <ranges>
#include <utility>

namespace kaixa::plugin::cmake::detail {
    namespace {
        std::string quote(const std::string_view value) {
            std::string equals;
            while (value.contains("]" + equals + "]"))
                equals += '=';

            return "[" + equals + "[" + std::string(value) + "]" + equals + "]";
        }

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

        std::string discovery_script(const TestOptions& test) {
            const std::string label = std::string(test_target_label_prefix) + test.target;
            std::string output;
            output += "set(_kaixa_test_executable "
                + quote("$<TARGET_FILE:" + test.target + ">") + ")\n";
            output += "set(_kaixa_test_prefix " + quote(test.name) + ")\n";
            output += "set(_kaixa_test_label " + quote(label) + ")\n";
            output += R"cmake(execute_process(
  COMMAND "${_kaixa_test_executable}" --kaixa-test-list
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
  add_test("${_kaixa_name}" "${_kaixa_test_executable}" --kaixa-test-run "${_kaixa_case}")cmake";
            for (const std::string& argument: test.arguments)
                output += " " + quote(argument);

            output += R"cmake()
  set_tests_properties("${_kaixa_name}" PROPERTIES LABELS "${_kaixa_test_label}")
endforeach()
)cmake";
            return output;
        }

        void generate_discovered_test(
            std::string& output,
            const TestOptions& test,
            const std::size_t index
        ) {
            const std::string variable = "_kaixa_discovery_" + std::to_string(index);
            const std::string filename = "kaixa-discovery-" + std::to_string(index);
            const std::string script = discovery_script(test);

            output += "set(" + variable
                + " \"${CMAKE_CURRENT_BINARY_DIR}/" + filename + "\")\n";
            output += "if(CMAKE_CONFIGURATION_TYPES)\n";
            output += "  file(GENERATE OUTPUT \"${" + variable
                + "}-$<CONFIG>.cmake\" CONTENT " + quote(script) + ")\n";
            output += "  file(WRITE \"${" + variable + "}.cmake\""
                + " \"include(\\\"${" + variable
                + "}-\\${CTEST_CONFIGURATION_TYPE}.cmake\\\")\\n\")\n";
            output += "else()\n";
            output += "  file(GENERATE OUTPUT \"${" + variable
                + "}.cmake\" CONTENT " + quote(script) + ")\n";
            output += "endif()\n";
            output += "set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES";
            output += " \"${" + variable + "}.cmake\")\n";
        }

        Result<Action*> find_build_action(
            BuildPlan& plan,
            const PackageNode& package
        ) {
            const auto action = std::ranges::find_if(plan.actions(), [&](const Action& candidate) {
                return candidate.package == package.id
                    && candidate.stage == ActionStage::build;
            });
            if (action == plan.actions().end()) {
                return std::unexpected(error(
                    "CMake test plan has no build action for package `" + package.name + "`"
                ));
            }

            return &*action;
        }
    }

    void generate_tests(
        std::string& output,
        const std::span<const TestOptions> tests
    ) {
        if (tests.empty())
            return;

        output += "enable_testing()\n\n";
        for (std::size_t index = 0; index < tests.size(); ++index) {
            const TestOptions& test = tests[index];
            if (test.discover) {
                generate_discovered_test(output, test, index);
                continue;
            }

            output += "add_test(NAME " + quote(test.name) + " COMMAND " + test.target;
            for (const std::string& argument: test.arguments)
                output += " " + quote(argument);

            output += ")\n";
            output += "set_tests_properties(" + quote(test.name) + " PROPERTIES LABELS "
                + quote(std::string(test_target_label_prefix) + test.target) + ")\n";
        }
    }

    Result<void> plan_tests(
        const Options& options,
        const PackageNode& package,
        const std::filesystem::path& build_directory,
        const std::string_view configuration,
        const TestRequest& request,
        BuildPlan& plan
    ) {
        if (request.target) {
            const auto target = std::ranges::find_if(
                options.targets,
                [&](const TargetOptions& candidate) {
                    return candidate.name == *request.target;
                }
            );
            if (target == options.targets.end()) {
                return std::unexpected(error(
                    "CMake target `" + *request.target + "` does not exist"
                ));
            }
            if (std::ranges::none_of(options.tests, [&](const TestOptions& test) {
                    return test.target == *request.target;
                })) {
                return std::unexpected(error(
                    "CMake target `" + *request.target + "` does not declare tests"
                ));
            }

            auto build = find_build_action(plan, package);
            if (!build)
                return std::unexpected(build.error());

            (*build)->argv.push_back("--target");
            (*build)->argv.push_back(*request.target);
        }

        Action action;
        action.description = "test " + package.name;
        action.argv = {
            "ctest",
            "--test-dir", build_directory.string(),
            "--build-config", std::string(configuration),
            "--output-on-failure"
        };
        if (request.filter) {
            action.argv.push_back("--tests-regex");
            action.argv.push_back(regex_escape(*request.filter));
        }
        if (request.target) {
            action.argv.push_back("--label-regex");
            action.argv.push_back(
                "^" + regex_escape(test_target_label_prefix)
                    + regex_escape(*request.target) + "$"
            );
        }
        action.working_directory = package.directory;
        action.package = package.id;
        action.stage = ActionStage::test;
        plan.add(std::move(action));
        return {};
    }
}
