// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace helix::test {

/// A scratch-dir path under the system temp dir that no concurrently running
/// test process can produce. Parallel Catch2 shards are separate processes
/// whose rand() sequences are identical, so a name built from rand() alone is
/// shared by the two shards a source file splits across, and their fixtures'
/// create/save/remove_all race on one directory - the losing destructor's
/// filesystem_error escapes as SIGABRT. getpid() separates the processes,
/// rand() separates fixtures within one process.
inline std::string unique_temp_dir(const std::string& prefix) {
    return std::filesystem::temp_directory_path().string() + "/" + prefix + "_" +
           std::to_string(::getpid()) + "_" + std::to_string(std::rand());
}

/// A scratch-file path under the system temp dir, same uniqueness contract.
inline std::string unique_temp_file(const std::string& prefix, const std::string& ext) {
    return unique_temp_dir(prefix) + "." + ext;
}

/// Just the <pid>_<rand> part, for names inside a directory the caller already
/// controls (cache keys, relative file names) rather than paths of their own.
inline std::string unique_suffix() {
    return std::to_string(::getpid()) + "_" + std::to_string(std::rand());
}

} // namespace helix::test
