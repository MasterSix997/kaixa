#include <kaixa/foundation/filesystem.hpp>

#include <fstream>
#include <iterator>
#include <system_error>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace kaixa {
    Result<std::string> read_file(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return std::unexpected(error_at({path.string(), 0, 0, {}}, "cannot open file"));

        std::string contents{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        if (file.bad())
            return std::unexpected(error_at({path.string(), 0, 0, {}}, "cannot read file"));

        return contents;
    }

    Result<void> write_file(const std::filesystem::path& path, const std::string_view contents) {
        std::error_code failure;
        const std::filesystem::path parent = path.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, failure);
            if (failure)
                return std::unexpected(error_at({path.string(), 0, 0, {}}, "cannot create parent directory: " + failure.message()));
        }

        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return std::unexpected(error_at({path.string(), 0, 0, {}}, "cannot open file for writing"));

        file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!file)
            return std::unexpected(error_at({path.string(), 0, 0, {}}, "cannot write file"));

        return {};
    }

    Result<void> write_file_atomic(const std::filesystem::path& path, const std::string_view contents) {
        const std::filesystem::path temporary = path.string() + ".kaixa-partial";
        auto written = write_file(temporary, contents);
        if (!written)
            return std::unexpected(written.error());

#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const std::error_code failure(static_cast<int>(GetLastError()), std::system_category());
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return std::unexpected(error("cannot atomically replace `" + path.string() + "`: " + failure.message()));
        }
#else
        std::error_code failure;
        std::filesystem::rename(temporary, path, failure);
        if (failure) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return std::unexpected(error("cannot atomically replace `" + path.string() + "`: " + failure.message()));
        }
#endif
        return {};
    }
}
