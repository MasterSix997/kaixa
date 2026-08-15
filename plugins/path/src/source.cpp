#include <kaixa/plugin/path/source.hpp>

#include <system_error>

namespace kaixa::plugin::path {
    namespace {
        class PathSourceDriver final : public SourceDriver {
        public:
            [[nodiscard]] SourceDriverInfo info() const override {
                return {"path", "opens an existing local source tree"};
            }

            [[nodiscard]] Result<std::optional<SourceTree>> locate(const SourceLocator& source, const SourceContext& context) const override {
                const std::vector<TableEntry>* options = source.options.as_table();
                if (!options)
                    return std::unexpected(error("path source options must be a table"));

                const Value* path = nullptr;
                for (const TableEntry& option: *options) {
                    if (option.key == "path") {
                        path = &option.value;
                        continue;
                    }

                    return std::unexpected(error_at(
                        option.value.location(),
                        "unknown path source option `" + option.key + "`"
                    ));
                }
                if (!path || !path->as_string()) {
                    return std::unexpected(error_at(
                        source.options.location(),
                        "path source requires a string `path`"
                    ));
                }

                std::filesystem::path directory = *path->as_string();
                if (directory.is_relative())
                    directory = context.requester / directory;

                std::error_code failure;
                directory = std::filesystem::absolute(directory, failure).lexically_normal();
                if (failure) {
                    return std::unexpected(error_at(
                        path->location(),
                        "cannot resolve path source `" + *path->as_string() + "`: "
                            + failure.message()
                    ));
                }

                const bool exists = std::filesystem::exists(directory, failure);
                if (failure) {
                    return std::unexpected(error_at(
                        path->location(),
                        "cannot inspect path source `" + directory.string() + "`: "
                            + failure.message()
                    ));
                }
                if (!exists)
                    return std::optional<SourceTree>{};

                if (!std::filesystem::is_directory(directory, failure) || failure) {
                    return std::unexpected(error_at(
                        path->location(),
                        failure
                            ? "cannot inspect path source `" + directory.string() + "`: "
                                + failure.message()
                            : "path source is not a directory: " + directory.string()
                    ));
                }

                return std::optional{SourceTree{directory, directory.generic_string()}};
            }
        };
    }

    std::unique_ptr<SourceDriver> make_source_driver() {
        return std::make_unique<PathSourceDriver>();
    }
}
