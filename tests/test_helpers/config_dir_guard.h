// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "scoped_env.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <signal.h>
#include <string>
#include <system_error>
#include <unistd.h>

namespace helix {

/// RAII guard: sandboxes HELIX_CONFIG_DIR into a scratch directory for the
/// duration of a test and restores the previous value (or unsets it) on
/// scope exit, including when a REQUIRE in between throws. Also restores
/// permissions before recursive removal, so a test that chmods a file inside
/// `dir` to exercise a permission-denied path still cleans up.
class ConfigDirGuard {
  public:
    explicit ConfigDirGuard(const std::string& suffix)
        : dir(dir_path_for(suffix, ::getpid())), env_("HELIX_CONFIG_DIR", dir.string().c_str()) {
        sweep_dead_sibling_dirs();
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        write_marker();
    }

    ~ConfigDirGuard() {
        restore_permissions_and_remove(dir);
    }

    ConfigDirGuard(const ConfigDirGuard&) = delete;
    ConfigDirGuard& operator=(const ConfigDirGuard&) = delete;

    std::filesystem::path dir;

  private:
    ScopedEnv env_;

    /// Directory names are <prefix><suffix>_<pid>. Both the constructor and
    /// the stale-directory sweep derive them from this one grammar.
    static constexpr const char* k_prefix = "helix_config_dir_guard_";

    /// Ownership marker written inside every directory this class creates.
    /// /tmp is world-writable, so a family name alone does not prove a
    /// directory is ours; the sweep only removes directories that contain
    /// this file as a regular file (not through a symlink).
    static constexpr const char* k_marker_name = ".helix_config_dir_guard";

    static std::filesystem::path dir_path_for(const std::string& suffix, long pid) {
        return std::filesystem::temp_directory_path() /
               (std::string(k_prefix) + suffix + "_" + std::to_string(pid));
    }

    /// True when @p name parses as <prefix><anything>_<digits> with a
    /// positive pid; suffixes may themselves contain underscores, so the pid
    /// is the tail after the FINAL underscore.
    static bool family_pid_of(const std::string& name, long& pid_out) {
        if (name.rfind(k_prefix, 0) != 0) {
            return false;
        }
        const size_t sep = name.rfind('_');
        if (sep == std::string::npos) {
            return false;
        }
        const std::string pid_str = name.substr(sep + 1);
        if (pid_str.empty() || pid_str.size() > 9 ||
            pid_str.find_first_not_of("0123456789") != std::string::npos) {
            return false;
        }
        const long pid = std::strtol(pid_str.c_str(), nullptr, 10);
        if (pid <= 0) {
            return false;
        }
        pid_out = pid;
        return true;
    }

    void write_marker() const {
        std::ofstream marker(dir / k_marker_name, std::ios::trunc);
        marker << ::getpid() << '\n';
        if (!marker) {
            // A guard directory without the marker is invisible to the sweep
            // forever, so a failed write is today's leak - say so where the
            // accumulation will eventually be investigated.
            spdlog::warn("[ConfigDirGuard] ownership marker would not write into {}; the "
                         "stale-dir sweep will never clean this directory",
                         dir.string());
        }
    }

    /// A shard killed by its per-shard timeout never runs the destructor, so
    /// every killed run strands one of these directories in /tmp. Remove
    /// family directories whose owning pid is provably dead AND that carry
    /// the ownership marker; leave everything else exactly as found.
    ///
    /// /tmp is world-writable, so the sweep assumes anything planted there
    /// could be hostile: a family-named entry that is not a real directory
    /// (a symlink to an arbitrary tree) is skipped without being followed.
    /// A live pid (a concurrent shard) is also left alone. Best effort
    /// throughout: /tmp turns over while the iterator walks it, and a
    /// mid-scan race just ends the sweep early.
    ///
    /// The marker requirement is deliberate and has a cost: directories
    /// stranded before markers existed do not carry one, so the sweep will
    /// never claim them - cleaning those is a one-time manual job, not
    /// something to widen the sweep back open for.
    ///
    /// Once per process: the walk covers all of /tmp and the guard is
    /// constructed per test case. A later shard is a separate process and
    /// still gets its sweep.
    static void sweep_dead_sibling_dirs() {
        static bool swept = false;
        if (swept) {
            return;
        }
        swept = true;
        try {
            std::error_code ec;
            for (const auto& entry :
                 std::filesystem::directory_iterator(std::filesystem::temp_directory_path(), ec)) {
                const std::string name = entry.path().filename().string();
                long pid = 0;
                if (!family_pid_of(name, pid)) {
                    continue;
                }
                if (::kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH) {
                    continue; // alive, or not provably dead
                }
                std::error_code not_dir;
                if (!std::filesystem::is_directory(entry.symlink_status(not_dir)) || not_dir) {
                    continue; // symlink or other non-directory: never follow
                }
                std::error_code owner_ec;
                const std::filesystem::file_status marker =
                    std::filesystem::symlink_status(entry.path() / k_marker_name, owner_ec);
                if (owner_ec || !std::filesystem::is_regular_file(marker)) {
                    continue; // no marker: not provably ours
                }
                restore_permissions_and_remove(entry.path());
            }
        } catch (const std::filesystem::filesystem_error&) {
        }
    }

    /// Permissions first, then removal: a killed or failing test may leave
    /// files chmod'd read-only behind, and the guard's own tests exercise
    /// exactly that path. @p path's own mode is restored first because a
    /// read-only root would make its children unremovable.
    ///
    /// Symlinked entries are not chmodmed, and each entry is re-stat'ed
    /// immediately before its chmod because the iterator's status was cached
    /// at readdir time. That narrows the swap-for-a-symlink race; it does
    /// not close it - an entry replaced after its check can still redirect
    /// that one chmod to the link's target. perm_options::nofollow would
    /// close it and is unusable here: this platform's fchmodat rejects
    /// AT_SYMLINK_NOFOLLOW (EINVAL on every call). Accepted residual risk
    /// for test-only code - the walk stays inside a directory the marker
    /// proved ours, the iterator never descends symlinked directories, and
    /// remove_all() unlinks a symlink itself rather than following it.
    static void restore_permissions_and_remove(const std::filesystem::path& path) {
        std::error_code ec;
        std::filesystem::permissions(path, std::filesystem::perms::owner_all, ec);
        for (auto& entry : std::filesystem::recursive_directory_iterator(path, ec)) {
            std::error_code link_ec;
            if (std::filesystem::is_symlink(
                    std::filesystem::symlink_status(entry.path(), link_ec)) ||
                link_ec) {
                continue; // a symlink, or gone since the walk passed it
            }
            std::filesystem::permissions(entry.path(), std::filesystem::perms::owner_all, ec);
        }
        std::filesystem::remove_all(path, ec);
    }
};

} // namespace helix
