#pragma once

#include <kaixa/foundation/diagnostic.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace kaixa {
    enum class ArchiveEntryKind {
        file,
        directory,
        link
    };

    struct ArchiveEntry {
        std::string path;
        ArchiveEntryKind kind = ArchiveEntryKind::file;
        std::uintmax_t size = 0;
    };

    class ArchiveBackend {
    public:
        virtual ~ArchiveBackend() = default;

        [[nodiscard]] virtual Result<void> create_archive(
            const std::filesystem::path& contents,
            const std::filesystem::path& destination
        ) const = 0;
        [[nodiscard]] virtual Result<std::vector<ArchiveEntry>> list_archive(const std::filesystem::path& archive) const = 0;
        [[nodiscard]] virtual Result<void> extract_archive(
            const std::filesystem::path& archive,
            const std::filesystem::path& destination
        ) const = 0;
    };

    [[nodiscard]] const ArchiveBackend& default_archive_backend();
}
