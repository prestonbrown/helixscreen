// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "scoped_env.h"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
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
        : dir(std::filesystem::temp_directory_path() /
              ("helix_config_dir_guard_" + suffix + "_" + std::to_string(::getpid()))),
          env_("HELIX_CONFIG_DIR", dir.string().c_str()) {
        sweep_dead_sibling_dirs();
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
    }

    ~ConfigDirGuard() {
        restore_permissions_and_remove(dir);
    }

    ConfigDirGuard(const ConfigDirGuard&) = delete;
    ConfigDirGuard& operator=(const ConfigDirGuard&) = delete;

    std::filesystem::path dir;

  private:
    ScopedEnv env_;

    /// A shard killed by its per-shard timeout never runs the destructor, so
    /// every killed run strands one of these directories in /tmp. Remove
    /// family directories whose owning pid is provably dead; a live pid (a
    /// concurrent shard) and any name that does not parse as ours are left
    /// alone. Best effort throughout: /tmp turns over while the iterator
    /// walks it, and a mid-scan race just ends the sweep early.
    void sweep_dead_sibling_dirs() const {
        const std::string prefix = "helix_config_dir_guard_";
        try {
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(dir.parent_path(), ec)) {
                const std::string name = entry.path().filename().string();
                if (name.rfind(prefix, 0) != 0) {
                    continue;
                }
                const std::string pid_str = name.substr(name.rfind('_') + 1);
                if (pid_str.empty() || pid_str.size() > 9 ||
                    pid_str.find_first_not_of("0123456789") != std::string::npos) {
                    continue;
                }
                const long pid = std::strtol(pid_str.c_str(), nullptr, 10);
                if (pid <= 0) {
                    continue;
                }
                if (::kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH) {
                    continue; // alive, or not provably dead
                }
                restore_permissions_and_remove(entry.path());
            }
        } catch (const std::filesystem::filesystem_error&) {
        }
    }

    /// Permissions first, then removal: a killed or failing test may leave
    /// files chmod'd read-only behind, and the guard's own tests exercise
    /// exactly that path.
    void restore_permissions_and_remove(const std::filesystem::path& path) const {
        std::error_code ec;
        for (auto& entry : std::filesystem::recursive_directory_iterator(path, ec)) {
            std::filesystem::permissions(entry.path(), std::filesystem::perms::owner_all, ec);
        }
        std::filesystem::remove_all(path, ec);
    }
};

} // namespace helix
