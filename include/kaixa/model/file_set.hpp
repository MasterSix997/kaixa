#pragma once

#include <kaixa/foundation/diagnostic.hpp>

#include <filesystem>
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

    [[nodiscard]] bool is_glob_pattern(std::string_view value) noexcept;
    [[nodiscard]] Result<std::vector<std::filesystem::path>> expand_file_set(
        const FileSet& files,
        const std::filesystem::path& root,
        const std::filesystem::path& relative_to
    );
}
