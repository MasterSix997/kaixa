#pragma once

#include <kaixa/config/value.hpp>
#include <kaixa/foundation/diagnostic.hpp>

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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

    using PolicyValidator = std::function<Result<void>(const Value& value)>;

    struct PolicyDefinition {
        std::string name;
        PolicyClass classification = PolicyClass::local;
        PolicyValidator validate;
    };

    class PolicySchema {
    public:
        PolicySchema() = default;

        [[nodiscard]] Result<void> add(PolicyDefinition definition);
        [[nodiscard]] Result<void> add(std::span<const PolicyDefinition> definitions);
        [[nodiscard]] const PolicyDefinition* find(std::string_view key) const noexcept;
        [[nodiscard]] bool empty() const noexcept { return m_definitions.empty(); }

    private:
        std::vector<PolicyDefinition> m_definitions;
    };

    [[nodiscard]] const PolicySchema& core_policy_schema();
    [[nodiscard]] Result<EffectivePolicy> resolve_policy_layers(
        std::span<const Value> layers,
        std::span<const std::string> active_features,
        const PolicyContext& context,
        const PolicySchema& schema
    );
    [[nodiscard]] bool policy_values_equal(const Value& left, const Value& right);
    [[nodiscard]] std::string canonical_policy(const EffectivePolicy& policy);
    [[nodiscard]] std::string policy_fingerprint(const EffectivePolicy& policy);

    [[nodiscard]] Result<void> expect_policy_boolean(const Value& value);
    [[nodiscard]] Result<void> expect_policy_string(const Value& value, std::string_view description);
    [[nodiscard]] Result<void> expect_policy_string_array(const Value& value);
    [[nodiscard]] Result<void> expect_policy_table(const Value& value, std::string_view description);
    [[nodiscard]] Result<void> expect_policy_positive_integer(const Value& value, std::string_view description);
}
