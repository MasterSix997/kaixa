#include "policies.hpp"

#include <string>
#include <utility>
#include <vector>

namespace kaixa::plugin::cmake {
    namespace {
        Result<void> validate_msvc_runtime(const Value& value) {
            auto expected = expect_policy_string(value, "an MSVC runtime name");
            if (!expected)
                return expected;

            const std::string& runtime = *value.as_string();
            if (runtime != "static" && runtime != "dynamic") {
                return std::unexpected(
                    error_at(value.location(), "unknown MSVC runtime `" + runtime + "`; expected `static` or `dynamic`")
                );
            }
            return {};
        }

        PolicyDefinition boolean_policy(std::string name, const PolicyClass classification) {
            return {std::move(name), classification, [](const Value& value) { return expect_policy_boolean(value); }};
        }

        PolicyDefinition string_array_policy(std::string name, const PolicyClass classification) {
            return {std::move(name), classification, [](const Value& value) { return expect_policy_string_array(value); }};
        }

        std::vector<PolicyDefinition> build_definitions() {
            std::vector<PolicyDefinition> definitions;
            definitions.push_back({"cxx", PolicyClass::floor, [](const Value& value) {
                                       return expect_policy_positive_integer(value, "an integer C++ language floor");
                                   }});
            definitions.push_back(boolean_policy("exceptions", PolicyClass::abi));
            definitions.push_back(boolean_policy("rtti", PolicyClass::abi));
            definitions.push_back({"msvc-runtime", PolicyClass::abi, validate_msvc_runtime});
            definitions.push_back(string_array_policy("sanitizers", PolicyClass::abi));
            definitions.push_back(boolean_policy("warnings-as-errors", PolicyClass::local));
            definitions.push_back(boolean_policy("iwyu", PolicyClass::local));
            definitions.push_back(boolean_policy("compiler-cache", PolicyClass::local));
            definitions.push_back({"warnings", PolicyClass::local, [](const Value& value) {
                                       return expect_policy_string(value, "a warning level");
                                   }});
            definitions.push_back(string_array_policy("precompiled-headers", PolicyClass::local));
            definitions.push_back({"defines", PolicyClass::local, [](const Value& value) {
                                       return expect_policy_table(value, "a definitions table");
                                   }});
            return definitions;
        }
    }

    std::span<const PolicyDefinition> native_policy_definitions() {
        static const std::vector<PolicyDefinition> definitions = build_definitions();
        return definitions;
    }
}
