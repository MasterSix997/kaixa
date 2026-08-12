#include "file_api.hpp"

#include <string>
#include <system_error>

namespace kaixa::plugin::cmake::detail {
    namespace {
        Result<bool> inspect_regular_file(const std::filesystem::path& path) {
            std::error_code failure;
            const bool exists = std::filesystem::exists(path, failure);
            if (failure) {
                return std::unexpected(error(
                    "cannot inspect `" + path.string() + "`: " + failure.message()
                ));
            }
            if (!exists)
                return false;

            const bool regular = std::filesystem::is_regular_file(path, failure);
            if (failure) {
                return std::unexpected(error(
                    "cannot inspect `" + path.string() + "`: " + failure.message()
                ));
            }
            return regular;
        }

        Result<std::filesystem::path> latest_index(const std::filesystem::path& reply) {
            std::error_code failure;
            const bool exists = std::filesystem::exists(reply, failure);
            if (failure) {
                return std::unexpected(error(
                    "cannot inspect CMake File API replies in `" + reply.string()
                        + "`: " + failure.message()
                ));
            }
            if (!exists)
                return {};

            std::filesystem::directory_iterator entries(reply, failure);
            if (failure) {
                return std::unexpected(error(
                    "cannot list CMake File API replies in `" + reply.string()
                        + "`: " + failure.message()
                ));
            }

            std::filesystem::path latest;
            for (const std::filesystem::directory_entry& entry: entries) {
                const std::string name = entry.path().filename().string();
                if (!name.starts_with("index-") || !name.ends_with(".json"))
                    continue;

                if (latest.empty() || name > latest.filename().string())
                    latest = entry.path();
            }
            return latest;
        }

        Result<bool> changed_after(
            const std::filesystem::path& input,
            const std::filesystem::file_time_type configured_at
        ) {
            auto regular = inspect_regular_file(input);
            if (!regular)
                return std::unexpected(regular.error());
            if (!*regular)
                return true;

            std::error_code failure;
            const auto modified = std::filesystem::last_write_time(input, failure);
            if (failure) {
                return std::unexpected(error(
                    "cannot read timestamp for `" + input.string() + "`: " + failure.message()
                ));
            }
            return modified > configured_at;
        }
    }

    std::filesystem::path file_api_query(const std::filesystem::path& build) {
        return build / ".cmake/api/v1/query/client-kaixa/cmakeFiles-v1";
    }

    Result<ActionState> configuration_state(
        const std::filesystem::path& build,
        const std::span<const std::filesystem::path> explicit_inputs
    ) {
        const std::filesystem::path cache = build / "CMakeCache.txt";
        const std::filesystem::path query = file_api_query(build);
        const std::filesystem::path reply = build / ".cmake/api/v1/reply";

        auto cache_is_regular = inspect_regular_file(cache);
        if (!cache_is_regular)
            return std::unexpected(cache_is_regular.error());
        auto query_is_regular = inspect_regular_file(query);
        if (!query_is_regular)
            return std::unexpected(query_is_regular.error());
        if (!*cache_is_regular || !*query_is_regular)
            return ActionState::required;

        auto index = latest_index(reply);
        if (!index)
            return std::unexpected(index.error());
        if (index->empty())
            return ActionState::required;

        std::error_code failure;
        const auto configured_at = std::filesystem::last_write_time(*index, failure);
        if (failure) {
            return std::unexpected(error(
                "cannot read CMake File API reply timestamp `" + index->string()
                    + "`: " + failure.message()
            ));
        }

        auto query_changed = changed_after(query, configured_at);
        if (!query_changed)
            return std::unexpected(query_changed.error());
        if (*query_changed)
            return ActionState::required;

        for (const std::filesystem::path& input: explicit_inputs) {
            auto changed = changed_after(input, configured_at);
            if (!changed)
                return std::unexpected(changed.error());
            if (*changed)
                return ActionState::required;
        }
        return ActionState::current;
    }
}
