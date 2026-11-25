// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "runtime_config.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <utility>

namespace helix::mock {

/// A directory of G-code files a test plants for the mock printer's gcodes
/// root, overlaid on assets/test_gcodes. That directory is shared by every
/// test process, and the mock's history totals scan all of it, so a file
/// planted there moves another process's numbers. Listings, metadata, reads
/// and deletes see both; the history scan sees only the shipped files.
/// Empty (the default) means the shipped directory alone.
/// HELIX_MOCK_GCODE_DIR plants a directory for a whole --test run.
inline std::string& planted_gcode_dir() {
    static std::string dir = [] {
        const char* env = std::getenv("HELIX_MOCK_GCODE_DIR");
        return std::string(env ? env : "");
    }();
    return dir;
}

inline void set_planted_gcode_dir(std::string dir) {
    planted_gcode_dir() = std::move(dir);
}

/// HELIX_MOCK_SCRATCH_DIR, or empty when unset: where the mock writes the files
/// a real Klipper host leaves in /tmp. helix-tests points it into its per-run
/// sandbox, so a run that dies leaves none of them in /tmp.
inline const std::string& scratch_dir_override() {
    static const std::string dir = [] {
        const char* env = std::getenv("HELIX_MOCK_SCRATCH_DIR");
        return std::string(env ? env : "");
    }();
    return dir;
}

/// The directory the mock's resonance CSVs and klippy socket stand-in live in.
inline const std::string& scratch_dir() {
    static const std::string dir =
        scratch_dir_override().empty() ? std::string("/tmp") : scratch_dir_override();
    return dir;
}

/// Where files uploaded to the mock's gcodes root land when no directory is
/// planted: one directory per process, so the uploads of parallel runs and
/// tests never meet, and the shipped directory is never written.
inline std::string uploaded_gcode_dir() {
    static const std::string dir =
        ((scratch_dir_override().empty() ? std::filesystem::temp_directory_path()
                                         : std::filesystem::path(scratch_dir_override())) /
         ("helix-mock-uploads-" + std::to_string(static_cast<long>(::getpid()))))
            .string();
    return dir;
}

/// Where a gcodes-root-relative file lives on disk: the planted copy when one
/// exists, else an uploaded one, else the shipped directory.
inline std::string gcode_disk_path(const std::string& relative) {
    const std::string& planted = planted_gcode_dir();
    for (const std::string& dir : {planted, uploaded_gcode_dir()}) {
        if (dir.empty()) {
            continue;
        }
        std::string path = dir + "/" + relative;
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            return path;
        }
    }
    return std::string(RuntimeConfig::TEST_GCODE_DIR) + "/" + relative;
}

} // namespace helix::mock
