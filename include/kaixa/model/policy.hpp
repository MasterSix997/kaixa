#pragma once

#include <kaixa/config/value.hpp>
#include <kaixa/foundation/diagnostic.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kaixa {
    enum class PolicyClass {
        local,
        abi,
        floor
    };

    struct PolicyContext {
        std::string profile = "debug";
        std::string target_os;
    };

    struct PolicySetting {
        std::string key;
        PolicyClass classification = PolicyClass::local;
        Value value;
        SourceLocation location;
    };

    struct EffectivePolicy {
        std::vector<PolicySetting> settings;

        [[nodiscard]] const PolicySetting* find(std::string_view key) const noexcept;
    };

    [[nodiscard]] PolicyClass policy_class(std::string_view key) noexcept;
    [[nodiscard]] Result<EffectivePolicy> resolve_policy_layers(
        std::span<const Value> layers,
        std::span<const std::string> active_features,
        const PolicyContext& context = {}
    );
    [[nodiscard]] bool policy_values_equal(const Value& left, const Value& right);
    [[nodiscard]] std::string canonical_policy(const EffectivePolicy& policy);
    [[nodiscard]] std::string policy_fingerprint(const EffectivePolicy& policy);
}
