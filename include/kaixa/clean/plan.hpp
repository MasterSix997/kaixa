#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace kaixa {
    struct GeneratedCleanFile {
        std::filesystem::path path;
        std::string marker;
    };

    class CleanPlan {
    public:
        void add(std::filesystem::path path);
        void generated_file(GeneratedCleanFile file);

        [[nodiscard]] std::span<const std::filesystem::path> paths() const noexcept {
            return m_paths;
        }
        [[nodiscard]] std::span<const GeneratedCleanFile> generated_files() const noexcept {
            return m_generated_files;
        }

        [[nodiscard]] bool empty() const noexcept {
            return m_paths.empty() && m_generated_files.empty();
        }

    private:
        std::vector<std::filesystem::path> m_paths;
        std::vector<GeneratedCleanFile> m_generated_files;
    };
}
