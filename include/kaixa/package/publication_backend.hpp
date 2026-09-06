#pragma once

#include <kaixa/foundation/diagnostic.hpp>

#include <filesystem>
#include <optional>
#include <string>

namespace kaixa {
    struct PackageUpload {
        std::string endpoint;
        std::optional<std::string> token_environment;
        std::filesystem::path metadata;
        std::filesystem::path archive;
        std::filesystem::path working_directory;
    };

    class PublicationBackend {
    public:
        virtual ~PublicationBackend() = default;

        [[nodiscard]] virtual Result<void> upload(const PackageUpload& request) const = 0;
    };
}
