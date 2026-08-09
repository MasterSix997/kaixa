#include <kaixa/config/build_configuration.hpp>

#include <kaixa/config/parser.hpp>

#include <algorithm>
#include <utility>

namespace kaixa {
    namespace {
        Diagnostic wrong_kind(
            SourceLocation location,
            const std::string_view expected,
            const ValueKind found
        ) {
            return error_at(
                std::move(location),
                "expected " + std::string(expected) + ", found "
                    + std::string(value_kind_name(found))
            );
        }

        Result<std::vector<std::string>> optional_string_array(
            TableReader& table,
            const std::string_view key
        ) {
            const Value* value = table.take(key);
            if (!value)
                return std::vector<std::string>{};

            const std::vector<Value>* array = value->as_array();
            if (!array)
                return std::unexpected(wrong_kind(table.location_of(key), "an array", value->kind()));

            std::vector<std::string> result;
            result.reserve(array->size());
            for (const Value& item: *array) {
                const std::string* text = item.as_string();
                if (!text)
                    return std::unexpected(wrong_kind(item.location(), "a string", item.kind()));
                if (text->empty())
                    return std::unexpected(error_at(item.location(), "configuration name cannot be empty"));
                result.push_back(*text);
            }
            return result;
        }

        Value merge_values(const Value& base, const Value& overlay) {
            const std::vector<TableEntry>* base_table = base.as_table();
            const std::vector<TableEntry>* overlay_table = overlay.as_table();
            if (!base_table || !overlay_table)
                return overlay;

            std::vector<TableEntry> merged = *base_table;
            for (const TableEntry& incoming: *overlay_table) {
                const auto existing = std::ranges::find_if(
                    merged,
                    [&](const TableEntry& entry) { return entry.key == incoming.key; }
                );
                if (existing == merged.end()) {
                    merged.push_back(incoming);
                } else {
                    existing->value = merge_values(existing->value, incoming.value);
                }
            }
            return Value::table(std::move(merged), overlay.location());
        }

        ResolverBuildConfiguration& resolver_configuration(
            EffectiveBuildConfiguration& configuration,
            const std::string_view resolver
        ) {
            const auto existing = std::ranges::find_if(
                configuration.resolvers,
                [&](const ResolverBuildConfiguration& candidate) {
                    return candidate.resolver == resolver;
                }
            );
            if (existing != configuration.resolvers.end())
                return *existing;
            return configuration.resolvers.emplace_back(std::string(resolver));
        }

        void select(std::vector<std::string>& selected, const std::string& name) {
            const auto duplicate = std::ranges::find(selected, name);
            if (duplicate != selected.end())
                selected.erase(duplicate);
            selected.push_back(name);
        }
    }

    const ResolverBuildConfiguration* EffectiveBuildConfiguration::find(
        const std::string_view resolver
    ) const noexcept {
        const auto result = std::ranges::find_if(
            resolvers,
            [&](const ResolverBuildConfiguration& candidate) {
                return candidate.resolver == resolver;
            }
        );
        return result == resolvers.end() ? nullptr : &*result;
    }

    Result<ConfigurationSet> read_configuration_set(TableReader& root) {
        ConfigurationSet result;
        auto build_result = root.optional_table("build");
        if (!build_result)
            return std::unexpected(build_result.error());
        if (!*build_result)
            return result;
        TableReader build = std::move(**build_result);

        auto defaults = optional_string_array(build, "default-configs");
        if (!defaults)
            return std::unexpected(defaults.error());
        result.defaults = std::move(*defaults);

        auto configurations_result = build.optional_table("configs");
        if (!configurations_result)
            return std::unexpected(configurations_result.error());
        if (!*configurations_result) {
            auto finished = build.finish();
            if (!finished)
                return std::unexpected(finished.error());
            return result;
        }

        TableReader configurations = std::move(**configurations_result);
        for (const TableEntry& entry: configurations.entries()) {
            auto definition_result = TableReader::bind(
                entry.value,
                join_config_path(configurations.path(), entry.key)
            );
            if (!definition_result)
                return std::unexpected(definition_result.error());
            TableReader definition = std::move(*definition_result);

            ConfigurationDefinition parsed;
            parsed.name = entry.key;
            parsed.location = entry.value.location();
            parsed.location.config_path = definition.path();
            if (parsed.name.empty())
                return std::unexpected(error_at(parsed.location, "configuration name cannot be empty"));

            auto profile = definition.optional_string("profile");
            if (!profile)
                return std::unexpected(profile.error());
            parsed.profile = std::move(*profile);

            auto resolvers_result = definition.optional_table("resolvers");
            if (!resolvers_result)
                return std::unexpected(resolvers_result.error());
            if (*resolvers_result) {
                TableReader resolvers = std::move(**resolvers_result);
                for (const TableEntry& resolver: resolvers.entries()) {
                    if (!resolver.value.is_table()) {
                        SourceLocation location = resolver.value.location();
                        location.config_path = join_config_path(resolvers.path(), resolver.key);
                        return std::unexpected(wrong_kind(
                            std::move(location),
                            "a resolver settings table",
                            resolver.value.kind()
                        ));
                    }
                    parsed.resolvers.push_back({resolver.key, resolver.value});
                }
                resolvers.take_all();
            }

            auto finished = definition.finish();
            if (!finished)
                return std::unexpected(finished.error());
            result.definitions.push_back(std::move(parsed));
        }
        configurations.take_all();
        auto finished = build.finish();
        if (!finished)
            return std::unexpected(finished.error());
        return result;
    }

    Result<ConfigurationSet> parse_configuration_file(const std::filesystem::path& path) {
        auto document = parse_file(path);
        if (!document)
            return std::unexpected(document.error());
        auto root_result = TableReader::bind(*document);
        if (!root_result)
            return std::unexpected(root_result.error());
        TableReader root = std::move(*root_result);

        auto configurations = read_configuration_set(root);
        if (!configurations)
            return std::unexpected(configurations.error());
        auto finished = root.finish();
        if (!finished)
            return std::unexpected(finished.error());
        return configurations;
    }

    Result<EffectiveBuildConfiguration> resolve_configurations(
        const std::span<const ConfigurationSet> layers,
        const std::span<const std::string> requested,
        const std::optional<std::string>& profile_override,
        const std::span<const ResolverArgumentOverride> argument_overrides
    ) {
        EffectiveBuildConfiguration result;
        for (const ConfigurationSet& layer: layers) {
            for (const std::string& name: layer.defaults)
                select(result.selected, name);
        }
        for (const std::string& name: requested)
            select(result.selected, name);

        for (const std::string& selected: result.selected) {
            bool found = false;
            for (const ConfigurationSet& layer: layers) {
                for (const ConfigurationDefinition& definition: layer.definitions) {
                    if (definition.name != selected)
                        continue;
                    found = true;
                    if (definition.profile)
                        result.profile = *definition.profile;

                    for (const ResolverConfigurationDefinition& resolver: definition.resolvers) {
                        ResolverBuildConfiguration& target = resolver_configuration(
                            result,
                            resolver.resolver
                        );
                        target.settings = target.settings
                            ? std::optional<Value>(merge_values(*target.settings, resolver.settings))
                            : std::optional<Value>(resolver.settings);
                    }
                }
            }
            if (!found)
                return std::unexpected(error("unknown build configuration `" + selected + "`"));
        }

        if (profile_override)
            result.profile = *profile_override;
        for (const ResolverArgumentOverride& override: argument_overrides) {
            ResolverBuildConfiguration& target = resolver_configuration(result, override.resolver);
            target.arguments.insert(
                target.arguments.end(),
                override.arguments.begin(),
                override.arguments.end()
            );
        }
        return result;
    }
}
