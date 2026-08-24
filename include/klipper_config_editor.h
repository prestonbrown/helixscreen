// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

class IMoonrakerAPI;
struct MoonrakerError;

namespace helix::system {

struct ConfigKey {
    std::string name;      // Key name (lowercased)
    std::string value;     // Raw value string (first line only for multi-line)
    std::string delimiter; // ":" or "=" — preserved for round-trip fidelity
    int line_number = 0;   // 0-indexed line number
    bool is_multiline = false;
    int end_line = 0; // Last line of value (for multi-line)
};

struct ConfigSection {
    std::string name;
    int line_start = 0; // Line of [section] header
    int line_end = 0;   // Last line before next section or EOF
    std::vector<ConfigKey> keys;
};

struct ConfigStructure {
    std::map<std::string, ConfigSection> sections;
    std::vector<std::string> includes;
    int save_config_line = -1;
    int total_lines = 0;

    std::optional<ConfigKey> find_key(const std::string& section, const std::string& key) const;
};

/// Which file a section was found in (for include resolution)
struct SectionLocation {
    std::string file_path; ///< Path relative to config root
    ConfigSection section; ///< Section info from that file
};

struct ConfigEdit {
    enum class Type { SET_VALUE, ADD_KEY, REMOVE_KEY };
    Type type;
    std::string key;
    std::string value; // ignored for REMOVE_KEY
};

class KlipperConfigEditor {
  public:
    using SuccessCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const std::string& error)>;
    using SectionMapCallback = std::function<void(std::map<std::string, SectionLocation>)>;

    ConfigStructure parse_structure(const std::string& content) const;

    /// Set a value for an existing key within a file's content
    /// Returns modified content, or std::nullopt if key not found
    std::optional<std::string> set_value(const std::string& content, const std::string& section,
                                         const std::string& key,
                                         const std::string& new_value) const;

    /// Add a new key to an existing section
    /// Returns modified content, or std::nullopt if section not found
    std::optional<std::string> add_key(const std::string& content, const std::string& section,
                                       const std::string& key, const std::string& value,
                                       const std::string& delimiter = ": ") const;

    /// Resolve all includes and build a section -> file mapping
    /// @param files Map of filename -> content (for unit testing without Moonraker)
    /// @param root_file Starting file to resolve from
    /// @param max_depth Maximum include recursion depth (default 5)
    /// @return Map of section_name -> SectionLocation
    std::map<std::string, SectionLocation>
    resolve_includes(const std::map<std::string, std::string>& files, const std::string& root_file,
                     int max_depth = 5) const;

    /// Comment out a key (prefix with #) — safer than deleting
    /// Returns modified content, or std::nullopt if key not found
    std::optional<std::string> remove_key(const std::string& content, const std::string& section,
                                          const std::string& key) const;

    // ========================================================================
    // Moonraker Integration — Async file operations
    // ========================================================================

    /// Load all config files from printer via Moonraker and resolve includes.
    /// Downloads printer.cfg + all included files, builds section map.
    /// Results are cached in section_map_ and file_cache_.
    void load_config_files(IMoonrakerAPI& api, SectionMapCallback on_complete,
                           ErrorCallback on_error);

    /// Edit a value in the correct config file with backup.
    /// Finds the file containing the section, backs it up, applies the edit,
    /// and uploads the modified content.
    void edit_value(IMoonrakerAPI& api, const std::string& section, const std::string& key,
                    const std::string& new_value, SuccessCallback on_success,
                    ErrorCallback on_error);

    /// Create backup of a config file (file.cfg -> file.cfg.helix_backup)
    void backup_file(IMoonrakerAPI& api, const std::string& file_path, SuccessCallback on_success,
                     ErrorCallback on_error);

    /// Restore all .helix_backup files to their original names
    void restore_backups(IMoonrakerAPI& api, SuccessCallback on_complete, ErrorCallback on_error);

    /// Delete all .helix_backup files (cleanup after successful edit)
    void cleanup_backups(IMoonrakerAPI& api, SuccessCallback on_complete);

    /// Get cached section map from last load_config_files() call
    std::map<std::string, SectionLocation> get_section_map() const;

    /// Get cached file content by path
    std::optional<std::string> get_cached_file(const std::string& path) const;

    /// Perform a safe config edit: edit value, restart firmware, monitor health.
    /// If Klipper fails to restart within the timeout, auto-restore backups
    /// and restart again.
    /// @param api Moonraker API instance
    /// @param section Config section to edit
    /// @param key Key to edit
    /// @param new_value New value
    /// @param on_success Called when edit is confirmed working
    /// @param on_error Called with error message if edit failed and was reverted
    /// @param restart_timeout_ms How long to wait for Klipper to come back (default 15000ms)
    void safe_edit_value(IMoonrakerAPI& api, const std::string& section, const std::string& key,
                         const std::string& new_value, SuccessCallback on_success,
                         ErrorCallback on_error, int restart_timeout_ms = 15000);

    /// Apply multiple edits to the same section atomically.
    /// All edits are applied to the in-memory content string before uploading.
    /// Returns modified content, or std::nullopt if section not found.
    std::optional<std::string> apply_edits(const std::string& content, const std::string& section,
                                           const std::vector<ConfigEdit>& edits) const;

    /// Safe multi-edit: load config, apply edits, backup, upload, restart, monitor health.
    /// If Klipper fails to restart, auto-restore backups.
    void safe_multi_edit(IMoonrakerAPI& api, const std::string& section,
                         const std::vector<ConfigEdit>& edits, SuccessCallback on_success,
                         ErrorCallback on_error, int restart_timeout_ms = 15000);

  private:
    /// Cached section map from last load_config_files()
    std::map<std::string, SectionLocation> section_map_;

    /// Cached file contents from last load
    std::map<std::string, std::string> file_cache_;

    /// Protects section_map_ and file_cache_
    mutable std::mutex cache_mutex_;

    /// Download a file and all its includes recursively
    /// @param api Moonraker API instance
    /// @param file_path Path relative to config root
    /// @param pending Shared counter of pending downloads
    /// @param on_all_done Called when all downloads complete (pending reaches 0)
    /// @param on_error Called on download failure
    void download_with_includes(IMoonrakerAPI& api, const std::string& file_path,
                                std::shared_ptr<std::atomic<int>> pending,
                                std::function<void()> on_all_done, ErrorCallback on_error);
};

} // namespace helix::system
