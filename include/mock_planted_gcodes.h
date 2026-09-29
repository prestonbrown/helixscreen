// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "runtime_config.h"

#include <filesystem>
#include <string>
#include <utility>

namespace helix::mock {

/// A directory of G-code files a test plants for the mock printer's gcodes
/// root, overlaid on assets/test_gcodes. That directory is shared by every
/// test process, and the mock's history totals scan all of it, so a file
/// planted there moves another process's numbers. Listings, metadata, reads
/// and deletes see both; the history scan sees only the shipped files.
/// Empty (the default) means the shipped directory alone.
inline std::string& planted_gcode_dir() {
    static std::string dir;
    return dir;
}

inline void set_planted_gcode_dir(std::string dir) {
    planted_gcode_dir() = std::move(dir);
}

/// Where a gcodes-root-relative file lives on disk: the planted copy when one
/// exists, else the shipped directory.
inline std::string gcode_disk_path(const std::string& relative) {
    const std::string& planted = planted_gcode_dir();
    if (!planted.empty()) {
        std::string path = planted + "/" + relative;
        if (std::filesystem::exists(path)) {
            return path;
        }
    }
    return std::string(RuntimeConfig::TEST_GCODE_DIR) + "/" + relative;
}

} // namespace helix::mock
