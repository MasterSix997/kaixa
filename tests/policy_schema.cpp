#include <test_support.hpp>

#include <kaixa/kaixa.hpp>
#include <kaixa/plugin/bundle.hpp>

#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

using kaixa::testing::TempDirectory;

namespace {
    // A resolver invented by this test: it registers a policy the core has never heard of.
    class ScriptResolver final : public kaixa::Resolver {
    public:
        [[nodiscard]] kaixa::ResolverInfo info() const override { return {"script", "a test resolver with its own policies"}; }

        [[nodiscard]] std::span<const kaixa::PolicyDefinition> policies() const override {
            static const std::vector<kaixa::PolicyDefinition> definitions = [] {
                std::vector<kaixa::PolicyDefinition> result;
                result.push_back(
                    {"script-runtime", kaixa::PolicyClass::abi, [](const kaixa::Value& value) -> kaixa::Result<void> {
                         auto expected = kaixa::expect_policy_string(value, "a script runtime name");
                         if (!expected)
                             return expected;

                         const std::string& runtime = *value.as_string();
                         if (runtime != "lua" && runtime != "wren") {
                             return std::unexpected(kaixa::error_at(value.location(), "unknown script runtime `" + runtime + "`"));
                         }
                         return {};
                     }}
                );
                result.push_back({"script-api", kaixa::PolicyClass::floor, [](const kaixa::Value& value) {
                                      return kaixa::expect_policy_positive_integer(value, "an integer script API floor");
                                  }});
                return result;
            }();
            return definitions;
        }

        [[nodiscard]] std::unique_ptr<kaixa::ResolverSession> start_session(const kaixa::Graph&) const override { return nullptr; }

        [[nodiscard]] kaixa::Result<void> plan(
            const kaixa::Graph&,
            const kaixa::ExtensionRegistry&,
            const kaixa::PackageNode&,
            const kaixa::BuildEnvironment&,
            std::span<const kaixa::ConfiguredPackageInstance>,
            const kaixa::BuildRequest&,
            kaixa::ExecutionPlan&,
            kaixa::ResolverSession&
        ) const override {
            return {};
        }

        [[nodiscard]] kaixa::Result<void> plan_tests(
            const kaixa::Graph&,
            const kaixa::ExtensionRegistry&,
            const kaixa::PackageNode&,
            const kaixa::BuildEnvironment&,
            std::span<const kaixa::ConfiguredPackageInstance>,
            const kaixa::TestRequest&,
            kaixa::ExecutionPlan&,
            kaixa::ResolverSession&
        ) const override {
            return {};
        }

        [[nodiscard]] kaixa::Result<std::vector<kaixa::BuildProduct>> products(
            const kaixa::Graph&,
            const kaixa::ExtensionRegistry&,
            const kaixa::PackageNode&,
            const kaixa::BuildEnvironment&,
            std::span<const kaixa::ConfiguredPackageInstance>,
            kaixa::ResolverSession&
        ) const override {
            return std::vector<kaixa::BuildProduct>{};
        }

        [[nodiscard]] kaixa::Result<void> plan_clean(
            const kaixa::Graph&,
            const kaixa::ExtensionRegistry&,
            const kaixa::PackageNode&,
            const kaixa::BuildEnvironment&,
            std::span<const kaixa::ConfiguredPackageInstance>,
            const kaixa::CleanRequest&,
            kaixa::CleanPlan&,
            kaixa::ResolverSession&
        ) const override {
            return {};
        }
    };

    kaixa::Value policy(std::initializer_list<kaixa::TableEntry> entries) {
        return kaixa::Value::table(std::vector<kaixa::TableEntry>(entries));
    }
}

KAIXA_TEST(an_extension_registers_validates_and_classifies_its_own_policies) {
    kaixa::ExtensionRegistry registry;
    const auto added = registry.add(std::make_unique<ScriptResolver>());
    context.check(added.has_value(), "a resolver registers its policies");
    if (!added) {
        context.fail(kaixa::format_diagnostic(added.error()));
        return;
    }

    const kaixa::PolicySchema& schema = registry.policy_schema();
    const kaixa::PolicyDefinition* runtime = schema.find("script-runtime");
    context.check(runtime != nullptr, "the extension policy is discoverable by key");
    if (runtime != nullptr)
        context.check(runtime->classification == kaixa::PolicyClass::abi, "the extension owns the classification");

    context.check(schema.find("profile") != nullptr, "core policies remain available");
    context.check(schema.find("cxx") == nullptr, "policies of an absent extension are not registered");

    const std::vector<kaixa::Value> layers{policy({{"script-runtime", "lua"}, {"script-api", 3}})};
    const auto effective = kaixa::resolve_policy_layers(layers, {}, {}, schema);
    context.check(effective.has_value(), "extension policies resolve without changing the core");
    if (!effective) {
        context.fail(kaixa::format_diagnostic(effective.error()));
        return;
    }

    const kaixa::PolicySetting* resolved = effective->find("script-runtime");
    context.check(resolved != nullptr, "the setting survives resolution");
    if (resolved != nullptr) {
        context.check(resolved->classification == kaixa::PolicyClass::abi, "the registered class reaches the effective policy");
        context.check_equal(*resolved->value.as_string(), std::string("lua"), "the value is preserved");
    }
    const kaixa::PolicySetting* floor = effective->find("script-api");
    context.check(floor != nullptr && floor->classification == kaixa::PolicyClass::floor, "floor policies keep their class");
}

KAIXA_TEST(an_extension_policy_is_validated_by_its_owner) {
    kaixa::ExtensionRegistry registry;
    const auto added = registry.add(std::make_unique<ScriptResolver>());
    context.check(added.has_value(), "the test resolver registers");
    if (!added)
        return;

    const std::vector<kaixa::Value> invalid_value{policy({{"script-runtime", "python"}})};
    const auto rejected = kaixa::resolve_policy_layers(invalid_value, {}, {}, registry.policy_schema());
    context.check(!rejected.has_value(), "the owning extension rejects an unknown value");
    if (!rejected)
        context.check_contains(rejected.error().message, "unknown script runtime `python`", "owner diagnostic is reported");

    const std::vector<kaixa::Value> wrong_kind{policy({{"script-api", "three"}})};
    const auto mistyped = kaixa::resolve_policy_layers(wrong_kind, {}, {}, registry.policy_schema());
    context.check(!mistyped.has_value(), "a floor policy must be an integer");

    const std::vector<kaixa::Value> unregistered{policy({{"script-optimizer", true}})};
    const auto unknown = kaixa::resolve_policy_layers(unregistered, {}, {}, registry.policy_schema());
    context.check(!unknown.has_value(), "an unregistered key is rejected");
    if (!unknown)
        context.check_contains(unknown.error().message, "unknown policy `script-optimizer`", "unknown policy diagnostic");
}

KAIXA_TEST(two_extensions_cannot_register_the_same_policy_key) {
    kaixa::ExtensionRegistry registry;
    const auto first = registry.add(std::make_unique<ScriptResolver>());
    context.check(first.has_value(), "the first registration succeeds");

    const auto second = registry.add(std::make_unique<ScriptResolver>());
    context.check(!second.has_value(), "a duplicate policy key is rejected at registration");
    if (!second) {
        context.check_contains(second.error().message, "script-runtime", "the conflicting key is named");
        context.check_contains(kaixa::format_diagnostic(second.error()), "script", "the offending resolver is named");
    }
}

KAIXA_TEST(the_bundled_registry_owns_the_native_policies) {
    const kaixa::ExtensionRegistry registry = kaixa::plugin::default_registry();
    const kaixa::PolicySchema& schema = registry.policy_schema();
    for (const std::string_view key: {"cxx", "exceptions", "rtti", "msvc-runtime", "sanitizers", "defines"}) {
        context.check(schema.find(key) != nullptr, std::string("`") + std::string(key) + "` is registered by an extension");
    }
    const kaixa::PolicyDefinition* cxx = schema.find("cxx");
    context.check(cxx != nullptr && cxx->classification == kaixa::PolicyClass::floor, "`cxx` stays a floor policy");
    const kaixa::PolicyDefinition* exceptions = schema.find("exceptions");
    context.check(exceptions != nullptr && exceptions->classification == kaixa::PolicyClass::abi, "`exceptions` stays an ABI policy");
}
