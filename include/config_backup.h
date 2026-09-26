// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

namespace helix::config_backup {

/// Atomically write data to a backup path (write-to-tmp + rename for crash safety).
/// Creates parent directories if needed. Returns true on success.
bool write_backup_file(const std::string& src_path, const std::string& backup_path);

/// Write a rolling backup to primary path, falling back to secondary on failure.
void write_rolling_backup(const std::string& src_path, const std::string& primary,
                          const std::string& fallback);

/// Find the first existing path from a priority-ordered list.
/// Returns the path if found, empty string otherwise.
std::string find_backup(const std::vector<std::string>& paths);

/// Restore a file from backup locations if the target is missing.
/// Creates parent directory if needed. Returns true if restored.
bool restore_from_backup(const std::string& target_path, const char* label,
                         const std::vector<std::string>& backup_paths);

/// Remove every backup file that exists in the priority-ordered lists.
/// A factory reset calls this so a later restore cannot resurrect the
/// pre-reset config from a surviving tier. Logs each removal at info.
void remove_backups(const std::vector<std::string>& backup_paths);

} // namespace helix::config_backup
