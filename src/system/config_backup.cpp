// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config_backup.h"

#include "helix_fs.h"
#include "spdlog/spdlog.h"
#include "text_io.h"

#include <cerrno>
#include <cstring>
#include <optional>
#include <sys/stat.h>

namespace hfs = helix::fs;

namespace helix::config_backup {

bool write_backup_file(const std::string& src_path, const std::string& backup_path,
                       std::string* why) {
    auto fail = [&](const char* step) {
        const int err = errno;
        if (why) {
            *why = std::string(step) + ": " + std::strerror(err);
        }
        // Debug only: write_rolling_backup() warns once if BOTH tiers fail, and a
        // primary-only failure (/var/lib not writable, the $HOME tier works) is normal.
        spdlog::debug("[Config] Backup to {} failed at {}: {}", backup_path, step,
                      std::strerror(err));
        return false;
    };

    struct stat st {};
    if (stat(src_path.c_str(), &st) != 0) {
        if (why) {
            *why = "no source";
        }
        return false; // Source doesn't exist, nothing to back up
    }

    // Ensure parent directory exists (create if needed for $HOME fallback)
    std::string parent{hfs::parent_path(backup_path)};
    if (!parent.empty() && !hfs::exists(parent)) {
        if (!hfs::create_directories(parent)) {
            return fail("mkdir");
        }
    }

    const auto data = helix::text_io::read_file(src_path);
    if (!data) {
        return fail("read");
    }
    if (!helix::text_io::write_file_atomic(backup_path, *data)) {
        return fail("write");
    }
    return true;
}

void write_rolling_backup(const std::string& src_path, const std::string& primary,
                          const std::string& fallback) {
    std::string primary_why;
    if (write_backup_file(src_path, primary, &primary_why)) {
        spdlog::trace("[Config] Backup written: {}", primary);
        return;
    }
    std::string fallback_why;
    if (write_backup_file(src_path, fallback, &fallback_why)) {
        spdlog::trace("[Config] Backup written (fallback): {}", fallback);
        return;
    }
    // Both failed — non-fatal, but worth knowing about. The reasons are what
    // tells a read-only mount from a foreign-owned directory in a user's bundle.
    spdlog::warn("[Config] Backup of {} failed at both primary ({}: {}) and fallback ({}: {})",
                 src_path, primary, primary_why, fallback, fallback_why);
}

std::string find_backup(const std::vector<std::string>& paths) {
    struct stat st {};
    for (const auto& p : paths) {
        if (stat(p.c_str(), &st) == 0) {
            return p;
        }
    }
    return {};
}

bool restore_from_backup(const std::string& target_path, const char* label,
                         const std::vector<std::string>& backup_paths) {
    struct stat st {};
    if (stat(target_path.c_str(), &st) == 0) {
        return false; // Target exists, no restore needed
    }

    std::string backup = find_backup(backup_paths);
    if (backup.empty()) {
        return false;
    }

    spdlog::warn("[Config] {} missing — restoring from backup: {}", label, backup);

    std::string parent_dir{hfs::parent_path(target_path)};
    if (!parent_dir.empty() && !hfs::exists(parent_dir)) {
        if (!hfs::create_directories(parent_dir)) {
            spdlog::error("[Config] Failed to create dir {}: {}", parent_dir, std::strerror(errno));
            return false;
        }
    }

    if (!hfs::copy_file(backup, target_path)) {
        spdlog::error("[Config] Failed to restore {}: {}", label, std::strerror(errno));
        return false;
    }
    spdlog::info("[Config] Restored {} from backup: {}", label, backup);
    return true;
}

void remove_backups(const std::vector<std::string>& backup_paths) {
    for (const auto& path : backup_paths) {
        if (hfs::remove(path)) {
            spdlog::info("[Config] Removed backup: {}", path);
        } else if (errno != ENOENT) {
            spdlog::warn("[Config] Failed to remove backup {}: {}", path, std::strerror(errno));
        }
    }
}

} // namespace helix::config_backup
