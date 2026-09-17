#include <kaixa/config/provider_configuration.hpp>

#include <kaixa/model/manifest.hpp>

#include <algorithm>
#include <utility>

namespace kaixa {
    Result<std::vector<ProviderDefinition>> read_provider_definitions(TableReader& root) {
        const Value* providers = root.take("provider");
        if (!providers)
            return std::vector<ProviderDefinition>{};

        const std::vector<Value>* definitions = providers->as_array();
        if (!definitions) {
            return std::unexpected(error_at(providers->location(), "provider definitions must be an array of tables"));
        }

        std::vector<ProviderDefinition> result;
        result.reserve(definitions->size());
        for (std::size_t index = 0; index < definitions->size(); ++index) {
            const Value& entry = (*definitions)[index];
            const std::string path = "provider." + std::to_string(index);
            auto definition_result = TableReader::bind(entry, path, root.sink());
            if (!definition_result)
                return std::unexpected(definition_result.error());

            TableReader definition = std::move(*definition_result);
            auto name = definition.string("name");
            auto driver = definition.string("driver");
            if (!name)
                return std::unexpected(name.error());
            if (!driver)
                return std::unexpected(driver.error());

            if (!is_valid_identifier(*name)) {
                return std::unexpected(error_at(definition.location_of("name"), "`" + *name + "` is not a valid provider name"));
            }

            if (!is_valid_identifier(*driver)) {
                return std::unexpected(error_at(definition.location_of("driver"), "`" + *driver + "` is not a valid provider driver name"));
            }

            if (std::ranges::any_of(result, [&](const ProviderDefinition& existing) { return existing.name == *name; })) {
                return std::unexpected(error_at(definition.location_of("name"), "provider `" + *name + "` is declared more than once"));
            }

            bool is_default = false;
            if (const Value* value = definition.take("default")) {
                const bool* boolean = value->as_boolean();
                if (!boolean) {
                    return std::unexpected(error_at(definition.location_of("default"), "provider `default` must be a boolean"));
                }
                is_default = *boolean;
            }

            Value options = definition.take_remaining();
            result.push_back({std::move(*name), std::move(*driver), is_default, std::move(options), entry.location(), {}});
        }

        return result;
    }
}
