#include <kaixa/model/policy.hpp>

#include <kaixa/config/value_operations.hpp>

#include <kaixa/model/graph.hpp>
#include <kaixa/model/package.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <map>
#include <utility>

namespace kaixa {
    namespace {
        Diagnostic wrong_kind(SourceLocation location, const std::string_view expected, const ValueKind found) {
            return wrong_value_kind(std::move(location), expected, found);
        }

        std::string target_os(const PolicyContext& context) {
            if (!context.target_os.empty())
                return context.target_os;

#ifdef _WIN32
            return "windows";
#elif defined(__APPLE__)
            return "macos";
#elif defined(__linux__)
            return "linux";
#else
            return "unknown";
#endif
        }

        Result<std::vector<std::string>> condition_values(const Value& value) {
            if (const std::string* text = value.as_string())
                return std::vector{*text};

            const std::vector<Value>* values = value.as_array();
            if (!values)
                return std::unexpected(wrong_kind(value.location(), "a condition string or string array", value.kind()));

            std::vector<std::string> result;
            result.reserve(values->size());
            for (const Value& item: *values) {
                const std::string* text = item.as_string();
                if (!text)
                    return std::unexpected(wrong_kind(item.location(), "a condition string", item.kind()));

                result.push_back(*text);
            }
            return result;
        }

        Result<bool> condition_matches(
            const Value& condition,
            const std::span<const std::string> active_features,
            const PolicyContext& context
        ) {
            const std::vector<TableEntry>* entries = condition.as_table();
            if (!entries)
                return std::unexpected(wrong_kind(condition.location(), "a condition table", condition.kind()));

            for (const TableEntry& entry: *entries) {
                auto expected = condition_values(entry.value);
                if (!expected)
                    return std::unexpected(expected.error());

                if (entry.key == "feature") {
                    if (std::ranges::none_of(*expected, [&](const std::string& feature) {
                            return std::ranges::find(active_features, feature) != active_features.end();
                        })) {
                        return false;
                    }
                } else if (entry.key == "profile") {
                    if (std::ranges::find(*expected, context.profile) == expected->end())
                        return false;

                } else if (entry.key == "target-os") {
                    if (std::ranges::find(*expected, target_os(context)) == expected->end())
                        return false;

                } else {
                    return std::unexpected(error_at(entry.value.location(), "unknown policy condition `" + entry.key + "`"));
                }
            }
            return true;
        }

        Result<void> assign_setting(EffectivePolicy& policy, const TableEntry& entry, const PolicySchema& schema) {
            const PolicyDefinition* definition = schema.find(entry.key);
            if (!definition) {
                return std::unexpected(error_at(entry.value.location(), "unknown policy `" + entry.key + "`")
                        .add_note("policies are registered by the extension that owns them"));
            }
            PolicySetting incoming{entry.key, definition->classification, entry.value, entry.value.location()};
            if (definition->classification == PolicyClass::floor && !incoming.value.as_integer()) {
                return std::unexpected(wrong_kind(incoming.location, "an integer policy floor", incoming.value.kind()));
            }
            if (definition->validate) {
                auto valid = definition->validate(incoming.value);
                if (!valid)
                    return std::unexpected(std::move(valid).error());
            }
            const auto existing = std::ranges::find(policy.settings, entry.key, &PolicySetting::key);
            if (existing == policy.settings.end())
                policy.settings.push_back(std::move(incoming));
            else
                *existing = std::move(incoming);

            return {};
        }

        Result<void> apply_entries(
            EffectivePolicy& policy,
            const std::vector<TableEntry>& entries,
            const bool accept_conditions,
            const PolicySchema& schema
        ) {
            for (const TableEntry& entry: entries) {
                if (entry.key == "when") {
                    if (!accept_conditions)
                        return std::unexpected(error_at(entry.value.location(), "nested policy conditions are not supported"));

                    continue;
                }

                if (entry.key == "if")
                    continue;

                auto assigned = assign_setting(policy, entry, schema);
                if (!assigned)
                    return std::unexpected(assigned.error());
            }
            return {};
        }

        void append_size(std::string& output, const std::size_t size) {
            output += std::to_string(size);
            output += ':';
        }

        void append_value(std::string& output, const Value& value) {
            switch (value.kind()) {
            case ValueKind::none: output += 'n'; break;
            case ValueKind::boolean: output += *value.as_boolean() ? "b1" : "b0"; break;
            case ValueKind::integer: output += 'i' + std::to_string(*value.as_integer()) + ';'; break;
            case ValueKind::floating: {
                output += 'f';
                std::array<char, 64> buffer{};
                const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), *value.as_floating());
                output.append(buffer.data(), converted.ptr);
                output += ';';
                break;
            }
            case ValueKind::string: {
                output += 's';
                append_size(output, value.as_string()->size());
                output += *value.as_string();
                break;
            }
            case ValueKind::array: {
                output += 'a';
                append_size(output, value.as_array()->size());
                for (const Value& item: *value.as_array())
                    append_value(output, item);

                break;
            }
            case ValueKind::table: {
                output += 't';
                std::vector<const TableEntry*> entries;
                entries.reserve(value.as_table()->size());
                for (const TableEntry& entry: *value.as_table())
                    entries.push_back(&entry);

                std::ranges::sort(entries, {}, [](const TableEntry* entry) { return entry->key; });
                append_size(output, entries.size());
                for (const TableEntry* entry: entries) {
                    append_size(output, entry->key.size());
                    output += entry->key;
                    append_value(output, entry->value);
                }
                break;
            }
            }
        }

        std::string value_text(const Value& value) {
            if (const bool* boolean = value.as_boolean())
                return *boolean ? "true" : "false";

            if (const std::int64_t* integer = value.as_integer())
                return std::to_string(*integer);

            if (const std::string* text = value.as_string())
                return "`" + *text + "`";

            std::string canonical;
            append_value(canonical, value);
            return canonical;
        }

        std::string fingerprint(const std::string_view text) {
            std::uint64_t hash = 14695981039346656037ull;
            for (const char character: text) {
                hash ^= static_cast<unsigned char>(character);
                hash *= 1099511628211ull;
            }

            std::array<char, 16> encoded{};
            const auto converted = std::to_chars(encoded.data(), encoded.data() + encoded.size(), hash, 16);
            return std::string(encoded.data(), converted.ptr);
        }

        void replace_setting(EffectivePolicy& policy, const PolicySetting& incoming) {
            const auto existing = std::ranges::find(policy.settings, incoming.key, &PolicySetting::key);
            if (existing == policy.settings.end())
                policy.settings.push_back(incoming);
            else
                *existing = incoming;
        }

        std::optional<Value> descriptor_abi(const PackageNode& package) {
            if (!package.descriptor())
                return std::nullopt;

            const Value* abi = package.descriptor()->find("abi");
            if (!abi)
                return std::nullopt;

            return *abi;
        }

        std::vector<PackageId> package_closure(const Graph& graph, const PackageId owner, const PackageTarget* target) {
            std::vector<PackageId> pending{owner};
            if (target && target->name) {
                const PackageNode& node = graph[owner];
                const auto dependencies = std::ranges::find(node.target_dependencies, *target->name, &PackageTargetDependencies::target);
                if (dependencies != node.target_dependencies.end()) {
                    pending.insert(pending.end(), dependencies->packages.begin(), dependencies->packages.end());
                }
            }

            std::vector<PackageId> result;
            while (!pending.empty()) {
                const PackageId package = pending.back();
                pending.pop_back();
                if (std::ranges::find(result, package) != result.end())
                    continue;

                result.push_back(package);
                const PackageNode& node = graph[package];
                pending.insert(pending.end(), node.dependencies.begin(), node.dependencies.end());
            }
            return result;
        }

        struct ContextInstance {
            PackageId package;
            std::vector<Value> layers;
            EffectivePolicy policy;
        };

        Value abi_layer(const EffectivePolicy& policy) {
            std::vector<TableEntry> entries;
            for (const PolicySetting& setting: policy.settings) {
                if (setting.classification == PolicyClass::abi && setting.key != "profile")
                    entries.push_back({setting.key, setting.value});
            }
            return Value::table(std::move(entries));
        }

        bool is_public_dependency(const Graph& graph, const PackageNode& package, const PackageId dependency) {
            if (!package.manifest())
                return false;

            const std::string& dependency_name = graph[dependency].name;
            const auto binding = std::ranges::find_if(package.manifest()->dependencies, [&](const DependencyBinding& candidate) {
                return candidate.request.package == dependency_name;
            });
            return binding != package.manifest()->dependencies.end() && binding->visibility == DependencyVisibility::public_dependency;
        }

        Result<std::vector<ContextInstance>> configure_context(
            const Graph& graph,
            const PackageId owner,
            const PackageTarget* target,
            const PolicyContext& context,
            const PolicySchema& schema,
            const std::string_view label
        ) {
            const PackageNode& owner_node = graph[owner];
            std::vector<Value> owner_layers = owner_node.policy_layers;
            if (target && target->policy)
                owner_layers.push_back(*target->policy);

            auto owner_policy = resolve_policy_layers(owner_layers, owner_node.active_features, context, schema);
            if (!owner_policy)
                return std::unexpected(owner_policy.error());

            const Value propagated = abi_layer(*owner_policy);
            const bool has_propagated = propagated.size() != 0;
            std::vector<ContextInstance> instances;
            for (const PackageId package: package_closure(graph, owner, target)) {
                const PackageNode& node = graph[package];
                std::vector<Value> layers = node.policy_layers;
                if (package == owner && target && target->policy)
                    layers.push_back(*target->policy);

                auto effective = resolve_policy_layers(layers, node.active_features, context, schema);
                if (!effective)
                    return std::unexpected(effective.error());

                const std::optional<Value> declared_abi = descriptor_abi(node);
                if (declared_abi) {
                    auto prebuilt = resolve_policy_layers(std::span<const Value>(&*declared_abi, 1), node.active_features, context, schema);
                    if (!prebuilt)
                        return std::unexpected(prebuilt.error());

                    for (const PolicySetting& requested: owner_policy->settings) {
                        if (requested.classification != PolicyClass::abi)
                            continue;

                        const PolicySetting* declared = prebuilt->find(requested.key);
                        if (declared && !policy_values_equal(declared->value, requested.value)) {
                            return std::unexpected(error_at(
                                declared->location,
                                "ABI policy conflict in artifact `"
                                    + std::string(label)
                                    + "` for `"
                                    + requested.key
                                    + "`: package `"
                                    + node.name
                                    + "` declares "
                                    + value_text(declared->value)
                                    + ", requested "
                                    + value_text(requested.value)
                            )
                                    .add_note("the requested value originates at `" + requested.location.config_path + "`"));
                        }
                    }
                    for (const PolicySetting& setting: prebuilt->settings) {
                        if (setting.classification == PolicyClass::abi)
                            replace_setting(*effective, setting);
                    }
                } else if (package != owner && has_propagated) {
                    layers.push_back(propagated);
                    effective = resolve_policy_layers(layers, node.active_features, context, schema);
                    if (!effective)
                        return std::unexpected(effective.error());
                }
                instances.push_back({package, std::move(layers), std::move(*effective)});
            }

            bool floor_changed = true;
            while (floor_changed) {
                floor_changed = false;
                for (ContextInstance& instance: instances) {
                    const PackageNode& node = graph[instance.package];
                    for (const PackageId dependency: node.dependencies) {
                        if (!is_public_dependency(graph, node, dependency))
                            continue;

                        const auto configured_dependency = std::ranges::find(instances, dependency, &ContextInstance::package);
                        if (configured_dependency == instances.end())
                            continue;

                        for (const PolicySetting& dependency_floor: configured_dependency->policy.settings) {
                            if (dependency_floor.classification != PolicyClass::floor)
                                continue;

                            const PolicySetting* current = instance.policy.find(dependency_floor.key);
                            if (current && *current->value.as_integer() >= *dependency_floor.value.as_integer())
                                continue;

                            replace_setting(instance.policy, dependency_floor);
                            floor_changed = true;
                        }
                    }
                }
            }

            std::map<std::string, std::pair<const PolicySetting*, const PackageNode*>> abi;
            for (const ContextInstance& instance: instances) {
                const PackageNode& node = graph[instance.package];
                for (const PolicySetting& setting: instance.policy.settings) {
                    if (setting.classification != PolicyClass::abi)
                        continue;

                    const auto [existing, inserted] = abi.emplace(setting.key, std::pair{&setting, &node});
                    if (!inserted && !policy_values_equal(existing->second.first->value, setting.value)) {
                        return std::unexpected(error_at(
                            setting.location,
                            "ABI policy conflict in artifact `"
                                + std::string(label)
                                + "` for `"
                                + setting.key
                                + "`: package `"
                                + existing->second.second->name
                                + "` requires "
                                + value_text(existing->second.first->value)
                                + ", package `"
                                + node.name
                                + "` requires "
                                + value_text(setting.value)
                        ));
                    }
                }
            }
            return instances;
        }

        std::string instance_identity(const PackageNode& package, const ContextInstance& instance) {
            std::string identity = package.name;
            identity += '\n';
            identity += package.resolver;
            identity += '\n';
            if (package.manifest() && package.manifest()->version)
                identity += package.manifest()->version->text;

            identity += '\n';
            if (package.source && package.source->identity)
                identity += *package.source->identity;
            else
                identity += package.directory.generic_string();

            std::vector<std::string> features = package.active_features;
            std::ranges::sort(features);
            for (const std::string& feature: features) {
                identity += '\n';
                identity += feature;
            }
            identity += '\n';
            identity += canonical_policy(instance.policy);
            return identity;
        }

        void append_instances(
            std::vector<ConfiguredPackageInstance>& output,
            const Graph& graph,
            std::vector<ContextInstance> instances,
            std::string context
        ) {
            for (ContextInstance& instance: instances) {
                const PackageNode& package = graph[instance.package];
                const std::string identity = instance_identity(package, instance);
                const auto existing = std::ranges::find_if(output, [&](const ConfiguredPackageInstance& candidate) {
                    return candidate.package == instance.package
                        && canonical_policy(candidate.policy) == canonical_policy(instance.policy)
                        && candidate.features == package.active_features;
                });
                if (existing != output.end()) {
                    if (std::ranges::find(existing->contexts, context) == existing->contexts.end())
                        existing->contexts.push_back(context);

                    continue;
                }

                output.push_back(
                    {instance.package,
                        package.name + '-' + fingerprint(identity),
                        package.active_features,
                        std::move(instance.layers),
                        std::move(instance.policy),
                        {context}}
                );
            }
        }
    }

    const PolicySetting* EffectivePolicy::find(const std::string_view key) const noexcept {
        const auto setting = std::ranges::find(settings, key, &PolicySetting::key);
        return setting == settings.end() ? nullptr : &*setting;
    }

    Result<void> PolicySchema::add(PolicyDefinition definition) {
        if (definition.name.empty())
            return std::unexpected(error("a policy definition requires a name"));

        const auto existing = std::ranges::find(m_definitions, definition.name, &PolicyDefinition::name);
        if (existing != m_definitions.end())
            return std::unexpected(error("policy `" + definition.name + "` is already registered"));

        m_definitions.push_back(std::move(definition));
        return {};
    }

    Result<void> PolicySchema::add(const std::span<const PolicyDefinition> definitions) {
        for (const PolicyDefinition& definition: definitions) {
            auto added = add(definition);
            if (!added)
                return added;
        }
        return {};
    }

    const PolicyDefinition* PolicySchema::find(const std::string_view key) const noexcept {
        const auto found = std::ranges::find(m_definitions, key, &PolicyDefinition::name);
        return found == m_definitions.end() ? nullptr : &*found;
    }

    Result<void> expect_policy_boolean(const Value& value) {
        if (!value.as_boolean())
            return std::unexpected(wrong_kind(value.location(), "a boolean policy value", value.kind()));

        return {};
    }

    Result<void> expect_policy_string(const Value& value, const std::string_view description) {
        if (!value.as_string())
            return std::unexpected(wrong_kind(value.location(), description, value.kind()));

        return {};
    }

    Result<void> expect_policy_string_array(const Value& value) {
        const std::vector<Value>* values = value.as_array();
        if (!values)
            return std::unexpected(wrong_kind(value.location(), "a string array", value.kind()));

        for (const Value& item: *values) {
            if (!item.as_string())
                return std::unexpected(wrong_kind(item.location(), "a string", item.kind()));
        }
        return {};
    }

    Result<void> expect_policy_table(const Value& value, const std::string_view description) {
        if (!value.is_table())
            return std::unexpected(wrong_kind(value.location(), description, value.kind()));

        return {};
    }

    Result<void> expect_policy_positive_integer(const Value& value, const std::string_view description) {
        const std::int64_t* integer = value.as_integer();
        if (!integer)
            return std::unexpected(wrong_kind(value.location(), description, value.kind()));

        if (*integer <= 0)
            return std::unexpected(error_at(value.location(), std::string(description) + " must be positive"));

        return {};
    }

    const PolicySchema& core_policy_schema() {
        static const PolicySchema schema = [] {
            PolicySchema built;
            auto added = built.add(PolicyDefinition{"profile", PolicyClass::abi, [](const Value& value) {
                                                        return expect_policy_string(value, "a build profile name");
                                                    }});
            static_cast<void>(added);
            return built;
        }();
        return schema;
    }

    Result<EffectivePolicy> resolve_policy_layers(
        const std::span<const Value> layers,
        const std::span<const std::string> active_features,
        const PolicyContext& context,
        const PolicySchema& schema
    ) {
        EffectivePolicy result;
        for (const Value& layer: layers) {
            const std::vector<TableEntry>* entries = layer.as_table();
            if (!entries)
                return std::unexpected(wrong_kind(layer.location(), "a policy table", layer.kind()));

            auto applied = apply_entries(result, *entries, true, schema);
            if (!applied)
                return std::unexpected(applied.error());

            const Value* conditions = layer.find("when");
            if (!conditions)
                continue;

            const std::vector<Value>* branches = conditions->as_array();
            if (!branches)
                return std::unexpected(wrong_kind(conditions->location(), "an array of conditional policy tables", conditions->kind()));

            for (const Value& branch: *branches) {
                const std::vector<TableEntry>* branch_entries = branch.as_table();
                if (!branch_entries)
                    return std::unexpected(wrong_kind(branch.location(), "a conditional policy table", branch.kind()));

                const Value* condition = branch.find("if");
                if (!condition)
                    return std::unexpected(error_at(branch.location(), "conditional policy entry requires `if`"));

                auto matches = condition_matches(*condition, active_features, context);
                if (!matches)
                    return std::unexpected(matches.error());

                if (!*matches)
                    continue;

                auto branch_applied = apply_entries(result, *branch_entries, false, schema);
                if (!branch_applied)
                    return std::unexpected(branch_applied.error());
            }
        }

        if (!result.find("profile")) {
            const SourceLocation profile_location{"build configuration", 0, 0, "profile"};
            const TableEntry profile{"profile", Value::string(context.profile, profile_location)};
            auto assigned = assign_setting(result, profile, schema);
            if (!assigned)
                return std::unexpected(assigned.error());
        }

        return result;
    }

    bool policy_values_equal(const Value& left, const Value& right) {
        std::string left_text;
        std::string right_text;
        append_value(left_text, left);
        append_value(right_text, right);
        return left_text == right_text;
    }

    std::string canonical_policy(const EffectivePolicy& policy) {
        std::vector<const PolicySetting*> settings;
        settings.reserve(policy.settings.size());
        for (const PolicySetting& setting: policy.settings)
            settings.push_back(&setting);

        std::ranges::sort(settings, {}, [](const PolicySetting* setting) { return setting->key; });
        std::string result;
        for (const PolicySetting* setting: settings) {
            append_size(result, setting->key.size());
            result += setting->key;
            result += setting->classification == PolicyClass::local ? 'l' : setting->classification == PolicyClass::abi ? 'a' : 'f';
            append_value(result, setting->value);
        }
        return result;
    }

    std::string policy_fingerprint(const EffectivePolicy& policy) {
        return fingerprint(canonical_policy(policy));
    }

    Result<std::vector<ConfiguredPackageInstance>> configure_package_instances(
        const Graph& graph,
        const PolicyContext& context,
        const PolicySchema& schema
    ) {
        std::vector<ConfiguredPackageInstance> result;
        for (const PackageNode& package: graph.nodes()) {
            auto configured = configure_context(graph, package.id, nullptr, context, schema, package.name + ":default");
            if (!configured)
                return std::unexpected(configured.error());

            append_instances(result, graph, std::move(*configured), package.name + ":default");

            if (!package.manifest())
                continue;

            for (const PackageTarget& target: package.targets) {
                if (!target.policy || !target.name)
                    continue;

                auto target_instances = configure_context(graph, package.id, &target, context, schema, *target.name);
                if (!target_instances)
                    return std::unexpected(target_instances.error());

                append_instances(result, graph, std::move(*target_instances), *target.name);
            }
        }
        return result;
    }

    const ConfiguredPackageInstance* find_configured_package_instance(
        const std::span<const ConfiguredPackageInstance> instances,
        const PackageId package,
        const std::string_view context
    ) noexcept {
        const auto instance = std::ranges::find_if(instances, [&](const ConfiguredPackageInstance& candidate) {
            return candidate.package == package && std::ranges::find(candidate.contexts, context) != candidate.contexts.end();
        });
        return instance == instances.end() ? nullptr : &*instance;
    }
}
