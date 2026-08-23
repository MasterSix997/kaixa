#pragma once

#include <kaixa/foundation/diagnostic.hpp>
#include <kaixa/model/manifest.hpp>

#include <filesystem>
#include <optional>
#include <string>

namespace kaixa {
    struct ManifestEdit {
        std::filesystem::path path;
        std::string before;
        std::string after;

        [[nodiscard]] bool changed() const noexcept { return before != after; }
    };

    [[nodiscard]] Result<ManifestEdit> add_manifest_dependency(
        const std::filesystem::path& manifest,
        std::string_view package,
        std::string_view requirement,
        const std::optional<std::string>& provider
    );
    [[nodiscard]] Result<ManifestEdit> remove_manifest_dependency(const std::filesystem::path& manifest, std::string_view package);
    [[nodiscard]] Result<void> apply_manifest_edit(const ManifestEdit& edit);

    struct PublishRequest {
        std::filesystem::path package;
        std::filesystem::path registry;
        std::optional<std::string> endpoint;
        std::optional<std::string> token_environment;
        std::optional<std::filesystem::path> prebuilt;
        bool dry_run = false;
    };

    struct PublishResult {
        std::string package;
        Version version;
        std::string archive;
        std::string integrity;
        bool prebuilt = false;
    };

    [[nodiscard]] Result<PublishResult> publish_package(const PublishRequest& request);
}
