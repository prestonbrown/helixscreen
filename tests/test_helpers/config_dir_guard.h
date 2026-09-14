// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "scoped_env.h"

#include <cstdlib>
#include <filesystem>
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
    explicit ConfigDirGuard(const std::string& suffix) : env_("HELIX_CONFIG_DIR") {
        dir = std::filesystem::temp_directory_path() /
              ("helix_config_dir_guard_" + suffix + "_" + std::to_string(::getpid()));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        setenv("HELIX_CONFIG_DIR", dir.string().c_str(), 1);
    }

    ~ConfigDirGuard() {
        std::error_code ec;
        for (auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
            std::filesystem::permissions(entry.path(), std::filesystem::perms::owner_all, ec);
        }
        std::filesystem::remove_all(dir, ec);
    }

    ConfigDirGuard(const ConfigDirGuard&) = delete;
    ConfigDirGuard& operator=(const ConfigDirGuard&) = delete;

    std::filesystem::path dir;

  private:
    ScopedEnv env_;
};

} // namespace helix
