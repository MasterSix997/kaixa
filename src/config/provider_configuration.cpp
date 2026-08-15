#include <kaixa/config/provider_configuration.hpp>

#include <kaixa/model/manifest.hpp>

#include <utility>

namespace kaixa {
    Result<std::vector<ProviderDefinition>> read_provider_definitions(TableReader& root) {
        auto providers_result = root.optional_table("providers");
        if (!providers_result)
            return std::unexpected(providers_result.error());
        if (!*providers_result)
            return std::vector<ProviderDefinition>{};

        TableReader providers = std::move(**providers_result);
        std::vector<ProviderDefinition> result;
        result.reserve(providers.entries().size());
        for (const TableEntry& entry: providers.entries()) {
            if (!is_valid_identifier(entry.key)) {
                return std::unexpected(error_at(
                    entry.value.location(),
                    "`" + entry.key + "` is not a valid provider name"
                ));
            }

            auto definition_result = TableReader::bind(
                entry.value,
                join_config_path(providers.path(), entry.key)
            );
            if (!definition_result)
                return std::unexpected(definition_result.error());

            TableReader definition = std::move(*definition_result);
            auto driver = definition.string("driver");
            if (!driver)
                return std::unexpected(driver.error());
            if (!is_valid_identifier(*driver)) {
                return std::unexpected(error_at(
                    definition.location_of("driver"),
                    "`" + *driver + "` is not a valid provider driver name"
                ));
            }

            bool is_default = false;
            if (const Value* value = definition.take("default")) {
                const bool* boolean = value->as_boolean();
                if (!boolean) {
                    return std::unexpected(error_at(
                        definition.location_of("default"),
                        "provider `default` must be a boolean"
                    ));
                }
                is_default = *boolean;
            }

            std::vector<TableEntry> options;
            for (const TableEntry& option: definition.entries()) {
                if (option.key != "driver" && option.key != "default")
                    options.push_back(option);
            }
            definition.take_all();
            result.push_back({
                entry.key,
                std::move(*driver),
                is_default,
                Value::table(std::move(options), entry.value.location()),
                entry.value.location()
            });
        }

        providers.take_all();
        return result;
    }
}
