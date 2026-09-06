#include "resolution_lock_coordinator.hpp"

#include <utility>

namespace kaixa::workspace_detail {
    Result<void> ResolutionLockCoordinator::read() {
        if (m_mode == LockMode::none)
            return {};

        auto lock = read_resolution_lock(m_lockfile);
        if (!lock)
            return std::unexpected(lock.error());

        if (*lock) {
            m_lock = std::move(**lock);
            return {};
        }
        if (m_mode == LockMode::locked || m_mode == LockMode::frozen) {
            return std::unexpected(error("lockfile does not exist: " + m_lockfile.string())
                    .add_note("run the command without `--locked` or `--frozen` to create it"));
        }
        return {};
    }

    Result<bool> ResolutionLockCoordinator::update(
        const Graph& graph,
        const std::span<const ConfiguredPackageInstance> instances,
        const PolicyContext& policy_context,
        const std::filesystem::path& context_directory
    ) {
        if (m_mode == LockMode::none)
            return false;

        const ResolutionLock current = capture_resolution_lock(graph, instances, policy_context, context_directory);
        if (m_mode == LockMode::locked || m_mode == LockMode::frozen) {
            auto valid = validate_resolution_lock(*m_lock, current);
            if (!valid)
                return std::unexpected(valid.error());

            return false;
        }

        if (m_write_lock) {
            ResolutionLock merged = m_lock ? merge_resolution_lock(*m_lock, current) : current;
            if (m_lock && resolution_locks_equal(*m_lock, merged))
                return false;

            return write_resolution_lock(m_lockfile, merged);
        }

        auto before = m_lock ? format_resolution_lock(*m_lock) : Result<std::string>{std::string{}};
        if (!before)
            return std::unexpected(before.error());

        ResolutionLock merged = m_lock ? merge_resolution_lock(*m_lock, current) : current;
        auto after = format_resolution_lock(merged);
        if (!after)
            return std::unexpected(after.error());

        return *before != *after;
    }
}
