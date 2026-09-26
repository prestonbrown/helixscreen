// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config_backup.h"

#include "spdlog/spdlog.h"

#include <cstdio>
#include <filesystem>
#include <sys/stat.h>

namespace fs = std::filesystem;

namespace helix::config_backup {

bool write_backup_file(const std::string& src_path, const std::string& backup_path) {
    struct stat st {};
    if (stat(src_path.c_str(), &st) != 0) {
        return false; // Source doesn't exist, nothing to back up
    }

    // Ensure parent directory exists (create if needed for $HOME fallback)
    fs::path parent = fs::path(backup_path).parent_path();
    if (!parent.empty() && !fs::exists(parent)) {
        std::error_code ec;
        fs::create_directories(parent, ec);
        if (ec) {
            return false;
        }
    }

    std::string tmp_path = backup_path + ".tmp";
    try {
        fs::copy_file(src_path, tmp_path, fs::copy_options::overwrite_existing);
        if (std::rename(tmp_path.c_str(), backup_path.c_str()) != 0) {
            std::remove(tmp_path.c_str());
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        std::remove(tmp_path.c_str());
        // Demoted to debug — write_rolling_backup() warns once if BOTH primary
        // and fallback fail. A primary-only failure (typical in dev: /var/lib
        // not writable, $HOME fallback succeeds) is not noteworthy.
        spdlog::debug("[Config] Backup to {} failed: {}", backup_path, e.what());
        return false;
    }
}

void write_rolling_backup(const std::string& src_path, const std::string& primary,
                          const std::string& fallback) {
    if (write_backup_file(src_path, primary)) {
        spdlog::trace("[Config] Backup written: {}", primary);
        return;
    }
    if (write_backup_file(src_path, fallback)) {
        spdlog::trace("[Config] Backup written (fallback): {}", fallback);
        return;
    }
    // Both failed — non-fatal, but worth knowing about.
    spdlog::warn("[Config] Backup of {} failed at both primary ({}) and fallback ({})", src_path,
                 primary, fallback);
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

    fs::path parent_dir = fs::path(target_path).parent_path();
    if (!parent_dir.empty() && !fs::exists(parent_dir)) {
        std::error_code ec;
        fs::create_directories(parent_dir, ec);
        if (ec) {
            spdlog::error("[Config] Failed to create dir {}: {}", parent_dir.string(),
                          ec.message());
            return false;
        }
    }

    try {
        fs::copy_file(backup, target_path);
        spdlog::info("[Config] Restored {} from backup: {}", label, backup);
        return true;
    } catch (const fs::filesystem_error& e) {
        spdlog::error("[Config] Failed to restore {}: {}", label, e.what());
        return false;
    }
}

void remove_backups(const std::vector<std::string>& backup_paths) {
    for (const auto& path : backup_paths) {
        std::error_code ec;
        if (fs::remove(path, ec)) {
            spdlog::info("[Config] Removed backup: {}", path);
        } else if (ec) {
            spdlog::warn("[Config] Failed to remove backup {}: {}", path, ec.message());
        }
    }
}

} // namespace helix::config_backup
