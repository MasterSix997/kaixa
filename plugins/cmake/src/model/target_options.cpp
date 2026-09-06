#include "target_options.hpp"

#include <kaixa/config/value_operations.hpp>

#include <algorithm>
#include <cstdint>
#include <utility>

namespace kaixa::plugin::cmake::detail {
    Diagnostic wrong_kind(SourceLocation location, const std::string_view expected, const ValueKind found) {
        return wrong_value_kind(std::move(location), expected, found);
    }

    Result<std::vector<std::string>> product_definitions(const Value& value, const std::filesystem::path& source_root) {
        const std::vector<TableEntry>* table = value.as_table();
        if (!table)
            return std::unexpected(wrong_kind(value.location(), "a definitions table", value.kind()));

        std::vector<std::string> result;
        result.reserve(table->size());
        for (const TableEntry& entry: *table) {
            if (const bool* boolean = entry.value.as_boolean()) {
                result.push_back(*boolean ? entry.key : entry.key + "=0");
            } else if (const std::int64_t* integer = entry.value.as_integer()) {
                result.push_back(entry.key + "=" + std::to_string(*integer));
            } else if (const std::string* text = entry.value.as_string()) {
                result.push_back(entry.key + "=" + *text);
            } else if (const Value* path = entry.value.find("path")) {
                const std::string* path_text = path->as_string();
                if (!path_text) {
                    return std::unexpected(wrong_kind(path->location(), "a path string", path->kind()));
                }
                result.push_back(entry.key + "=" + (source_root / std::filesystem::path(*path_text)).lexically_normal().generic_string());
            } else {
                return std::unexpected(error_at(entry.value.location(), "definition `" + entry.key + "` has an unsupported value"));
            }
        }
        return result;
    }

    Result<void> apply_policy(TargetOptions& target, const EffectivePolicy& policy, const std::filesystem::path& source_root) {
        if (const PolicySetting* cxx = policy.find("cxx")) {
            const std::int64_t floor = *cxx->value.as_integer();
            if (!target.cxx_standard || *target.cxx_standard < floor)
                target.cxx_standard = floor;
        }

        if (const PolicySetting* runtime = policy.find("msvc-runtime")) {
            const std::string& value = *runtime->value.as_string();
            target.msvc_runtime = value == "static" ? MsvcRuntime::static_runtime : MsvcRuntime::dynamic_runtime;
        }

        if (const PolicySetting* warnings = policy.find("warnings")) {
            const std::string& level = *warnings->value.as_string();
            if (level != "off" && level != "default") {
                target.compile_options.push_back("$<$<CXX_COMPILER_ID:MSVC>:/W4>");
                target.compile_options.push_back("$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>");
                target.compile_options.push_back("$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wextra>");
            }
            if (level == "pedantic") {
                target.compile_options.push_back("$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wpedantic>");
                target.compile_options.push_back("$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wconversion>");
                target.compile_options.push_back("$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wshadow>");
            } else if (level != "off" && level != "default" && level != "strict") {
                return std::unexpected(error_at(warnings->location, "unknown warning policy `" + level + "`"));
            }
        }

        if (const PolicySetting* errors = policy.find("warnings-as-errors"); errors && *errors->value.as_boolean()) {
            target.compile_options.push_back("$<$<CXX_COMPILER_ID:MSVC>:/WX>");
            target.compile_options.push_back("$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Werror>");
        }

        if (const PolicySetting* exceptions = policy.find("exceptions")) {
            if (*exceptions->value.as_boolean()) {
                target.compile_options.push_back("$<$<CXX_COMPILER_ID:MSVC>:/EHsc>");
                target.compile_options.push_back("$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-fexceptions>");
            } else {
                target.compile_options.push_back("$<$<CXX_COMPILER_ID:MSVC>:/EHs-c->");
                target.compile_options.push_back("$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-fno-exceptions>");
                target.compile_definitions.push_back("$<$<CXX_COMPILER_ID:MSVC>:_HAS_EXCEPTIONS=0>");
            }
        }

        if (const PolicySetting* rtti = policy.find("rtti")) {
            if (*rtti->value.as_boolean()) {
                target.compile_options.push_back("$<$<CXX_COMPILER_ID:MSVC>:/GR>");
                target.compile_options.push_back("$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-frtti>");
            } else {
                target.compile_options.push_back("$<$<CXX_COMPILER_ID:MSVC>:/GR->");
                target.compile_options.push_back("$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-fno-rtti>");
            }
        }

        if (const PolicySetting* sanitizers = policy.find("sanitizers")) {
            std::string value;
            for (const Value& sanitizer: *sanitizers->value.as_array()) {
                if (!value.empty())
                    value += ',';

                value += *sanitizer.as_string();
            }
            if (!value.empty()) {
                const std::string option = "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-fsanitize=" + value + ">";
                target.compile_options.push_back(option);
                target.link_options.push_back(option);
            }
        }

        if (const PolicySetting* headers = policy.find("precompiled-headers")) {
            for (const Value& header: *headers->value.as_array())
                target.precompiled_headers.push_back(*header.as_string());
        }

        if (const PolicySetting* defines = policy.find("defines")) {
            auto values = product_definitions(defines->value, source_root);
            if (!values)
                return std::unexpected(values.error());

            target.compile_definitions
                .insert(target.compile_definitions.end(), std::make_move_iterator(values->begin()), std::make_move_iterator(values->end()));
        }
        return {};
    }
}
