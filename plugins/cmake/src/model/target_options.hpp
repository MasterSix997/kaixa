#pragma once

#include <model/project_model.hpp>

#include <kaixa/config/table_reader.hpp>
#include <kaixa/foundation/diagnostic.hpp>
#include <kaixa/model/policy.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace kaixa::plugin::cmake::detail {
    // Typed transformations over a CMake target. This phase never interprets a manifest table:
    // it receives values the schema phase already read and folds them into target options.
    [[nodiscard]] Diagnostic wrong_kind(SourceLocation location, std::string_view expected, ValueKind found);
    [[nodiscard]] Result<std::vector<std::string>> product_definitions(const Value& value, const std::filesystem::path& source_root);
    [[nodiscard]] Result<void> apply_policy(TargetOptions& target, const EffectivePolicy& policy, const std::filesystem::path& source_root);
}
