#pragma once

#include <kaixa/model/package.hpp>
#include <kaixa/workspace/resolution_lock.hpp>

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace kaixa::workspace_detail {
    // Owns everything about the lockfile: the mode, the path, the loaded lock and the unlock
    // selection. Package loading routes read it through this object and never write it.
    class ResolutionLockCoordinator {
    public:
        ResolutionLockCoordinator() = default;

        ResolutionLockCoordinator(
            const LockMode mode,
            std::filesystem::path lockfile,
            const std::span<const std::string> unlocked_packages,
            const bool unlock_all,
            const bool write_lock
        )
            : m_mode(mode)
            , m_lockfile(std::move(lockfile))
            , m_unlocked_packages(unlocked_packages)
            , m_unlock_all(unlock_all)
            , m_write_lock(write_lock) {}

        void adopt_default_lockfile(const std::filesystem::path& context_directory) {
            if (m_lockfile.empty())
                m_lockfile = context_directory / "Kaixa.lock";
        }

        [[nodiscard]] Result<void> read();
        [[nodiscard]] Result<bool> update(
            const Graph& graph,
            std::span<const ConfiguredPackageInstance> instances,
            const PolicyContext& policy_context,
            const std::filesystem::path& context_directory
        );

        [[nodiscard]] LockMode mode() const noexcept { return m_mode; }
        [[nodiscard]] bool offline() const noexcept { return m_mode == LockMode::frozen; }
        [[nodiscard]] const std::filesystem::path& lockfile() const noexcept { return m_lockfile; }
        [[nodiscard]] const ResolutionLock* lock() const noexcept { return m_lock ? &*m_lock : nullptr; }
        [[nodiscard]] std::span<const std::string> unlocked_packages() const noexcept { return m_unlocked_packages; }
        [[nodiscard]] bool unlock_all() const noexcept { return m_unlock_all; }

    private:
        LockMode m_mode = LockMode::none;
        std::filesystem::path m_lockfile;
        std::span<const std::string> m_unlocked_packages;
        bool m_unlock_all = false;
        bool m_write_lock = true;
        std::optional<ResolutionLock> m_lock;
    };
}
