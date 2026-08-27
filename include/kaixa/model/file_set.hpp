#pragma once

#include <kaixa/foundation/diagnostic.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace kaixa {
    struct FileSet {
        std::vector<std::string> include;
        std::vector<std::string> exclude;
        std::vector<std::filesystem::path> files;
        SourceLocation location;
    };

    class FileCatalog {
    public:
        FileCatalog();
        ~FileCatalog();

        FileCatalog(const FileCatalog&) = delete;
        FileCatalog& operator=(const FileCatalog&) = delete;
        FileCatalog(FileCatalog&&) noexcept;
        FileCatalog& operator=(FileCatalog&&) noexcept;

        [[nodiscard]] Result<std::vector<std::filesystem::path>> expand(
            const FileSet& files,
            const std::filesystem::path& root,
            const std::filesystem::path& relative_to,
            bool allow_unmatched = false
        );
        [[nodiscard]] std::size_t directories_read() const noexcept;
        [[nodiscard]] std::size_t entries_read() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };

    [[nodiscard]] bool is_glob_pattern(std::string_view value) noexcept;
    [[nodiscard]] Result<std::vector<std::filesystem::path>> expand_file_set(
        const FileSet& files,
        const std::filesystem::path& root,
        const std::filesystem::path& relative_to,
        bool allow_unmatched = false,
        FileCatalog* catalog = nullptr
    );
}
