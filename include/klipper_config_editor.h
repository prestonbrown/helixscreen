// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "klippy_state.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "hv/json.hpp"

namespace helix {
class IMoonrakerAPI;
enum class PluginFeatureState;
} // namespace helix
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
    /// SET_VALUE needs the key; ADD_KEY sets or adds it; ADD_IF_ABSENT adds it
    /// only where the section does not already name it, so a value the user
    /// chose survives; REMOVE_KEY of an absent key is skipped.
    enum class Type { SET_VALUE, ADD_KEY, ADD_IF_ABSENT, REMOVE_KEY };
    Type type;
    std::string key;
    std::string value; // ignored for REMOVE_KEY
};

/// True when every edit is an ADD_KEY, i.e. the whole list reads "make sure this
/// key says X" and carries no assumption that the section already exists. An
/// empty list is false — it asserts nothing about any section.
bool all_add_key(const std::vector<ConfigEdit>& edits);

/// The config-root directory HelixScreen's config backups live in, each at its
/// file's own relative path. Klipper's include globs never match a dot
/// directory, so no [include dir/*] reads a backup as config.
inline constexpr const char* kConfigBackupDir = ".helix_backups";

/// Where the file-API path backs config file @p file up:
/// .helix_backups/<file>.helix_backup.
std::string config_backup_path(const std::string& file);

/// The config file a config_backup_path() names; nullopt for any other path.
std::optional<std::string> config_backup_original(const std::string& path);

/// The file a file-API edit list for @p section goes to: the file of the
/// section's last definition in @p section_map, else the root (printer.cfg)
/// when no file defines the section and every edit is ADD_KEY. An ADD_KEY of
/// an option only the root's SAVE_CONFIG block defines also goes to the root:
/// Klipper's SAVE_CONFIG comments out a root definition of an option it saves,
/// but refuses one in an included file. Empty when no file can take the list.
std::string config_edit_target_file(const std::map<std::string, SectionLocation>& section_map,
                                    const std::string& root_content, const std::string& section,
                                    const std::vector<ConfigEdit>& edits);

/// A value a safe edit expects Klipper's configfile.settings to hold once the
/// restart is done. A restart that leaves it otherwise failed the edit: the
/// value was shadowed (a SAVE_CONFIG copy on a build that lets the block win)
/// or the restart never happened.
struct ExpectedSetting {
    std::string section;
    std::string option;
    std::string value;
};

/// Whether configfile.settings @p settings holds every @p expected value.
/// Klipper keys settings by lowercased section and option; a string compares
/// ignoring case, a number by value. On false, @p mismatch (when given) says
/// which value is off.
bool settings_hold(const nlohmann::json& settings, const std::vector<ExpectedSetting>& expected,
                   std::string* mismatch = nullptr);

/// What the FIRMWARE_RESTART monitor makes of one printer.info poll.
enum class RestartVerdict {
    Waiting, ///< poll again
    Ready,   ///< Klippy is ready on the restarted config
    Failed,  ///< the restart left Klippy in error, or not ready in time
};

/// The FIRMWARE_RESTART monitor's memory across polls. Right after the restart
/// is accepted, printer.info can still answer with the old Klippy's state, so
/// a READY or ERROR counts only once a poll has seen the restart (Klippy
/// starting, or Moonraker without a Klippy) or `settle_ms` have passed.
struct RestartWatch {
    int settle_ms = 2000;
    /// How long Klippy has to reach READY or ERROR.
    int timeout_ms = 15000;
    /// Extra time a Klippy still starting up (or still away) is given past the
    /// timeout: a slow MCU handshake is not a failed config.
    int startup_allowance_ms = 15000;
    bool saw_restart = false;
};

/// The watch a monitor with @p restart_timeout_ms starts from.
RestartWatch restart_watch_for(int restart_timeout_ms);

/// Judge one poll @p elapsed_ms after the restart was accepted. @p state is
/// nullopt when Moonraker has no Klippy ("disconnected") or did not answer.
RestartVerdict judge_restart_poll(RestartWatch& watch, std::optional<KlippyState> state,
                                  int elapsed_ms);

/// The HelixPrint plugin feature that reads and writes the config
/// (server.helix.config / server.helix.config_edit), and the contract this
/// editor speaks.
inline constexpr const char* kConfigEditFeature = "config_edit";
// Contract 2 reads `edits` as the objects this editor sends; a contract-1
// plugin reads them with Moonraker's get_list and refuses every call (#2144).
inline constexpr int kConfigEditFeatureMinVersion = 2;

/// Who writes a config edit: the plugin, atomically and in the file and line
/// that define each option, or the screen over Moonraker's file API.
enum class ConfigEditRoute { Plugin, FileApi };

/// The route for plugin_feature(kConfigEditFeature, kConfigEditFeatureMinVersion).
ConfigEditRoute config_edit_route(PluginFeatureState state);

/// What the plugin path makes of an edit list, given the plugin's read of the
/// section (a server.helix.config result).
struct PluginConfigEditPlan {
    enum class Kind {
        Edits,   ///< send `edits` to server.helix.config_edit
        FileApi, ///< an option lives in the SAVE_CONFIG block: the file path serves it
        Error,   ///< the list cannot apply; `error` says why
    };
    Kind kind = Kind::Error;
    nlohmann::json edits = nlohmann::json::array();
    std::string error;
};

/// The server.helix.config_edit edit list for @p edits on @p section, each
/// carrying the value it expects to replace, read from @p config. Mirrors
/// apply_edits(): SET_VALUE needs the key, ADD_KEY sets or adds, ADD_IF_ABSENT
/// adds only an absent key, REMOVE_KEY of an absent key is skipped, and only an
/// all-ADD_KEY list may create a section.
PluginConfigEditPlan plan_plugin_config_edits(const std::string& section,
                                              const std::vector<ConfigEdit>& edits,
                                              const nlohmann::json& config);

/// The backups a server.helix.config_edit result names, as
/// (backup path, file path) pairs relative to the config root.
std::vector<std::pair<std::string, std::string>>
plugin_config_edit_backups(const nlohmann::json& result);

class KlipperConfigEditor {
  public:
    using FeatureStateSource = std::function<PluginFeatureState()>;

    KlipperConfigEditor();

    /// Cancels any in-flight FIRMWARE_RESTART health monitor and waits for it to
    /// unwind. The monitor polls on an HttpExecutor::fast() worker while holding
    /// `this` and the IMoonrakerAPI by reference, and this editor is a member of
    /// InputShaperPanel / PIDCalibrationPanel — both destroyed by
    /// StaticPanelRegistry::destroy_all() at shutdown, long before the executor
    /// workers stop. Without this join the monitor keeps polling a freed editor.
    ~KlipperConfigEditor();

    using SuccessCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const std::string& error)>;
    using SectionMapCallback = std::function<void(std::map<std::string, SectionLocation>)>;

    static ConfigStructure parse_structure(const std::string& content);

    /// Set a value for an existing key within a file's content
    /// Returns modified content, or std::nullopt if key not found
    static std::optional<std::string> set_value(const std::string& content,
                                                const std::string& section, const std::string& key,
                                                const std::string& new_value);

    /// Add a new key to an existing section
    /// Returns modified content, or std::nullopt if section not found
    static std::optional<std::string> add_key(const std::string& content,
                                              const std::string& section, const std::string& key,
                                              const std::string& value,
                                              const std::string& delimiter = ": ");

    /// Resolve all includes and build a section -> file mapping
    /// @param files Map of filename -> content (for unit testing without Moonraker)
    /// @param root_file Starting file to resolve from
    /// @param max_depth Maximum include recursion depth (default 5)
    /// @return Map of section_name -> SectionLocation
    std::map<std::string, SectionLocation>
    resolve_includes(const std::map<std::string, std::string>& files, const std::string& root_file,
                     int max_depth = 5) const;

    /// Append a new, empty [section] header to a file's content.
    /// Inserted before the `#*# <--- SAVE_CONFIG` block when one is present,
    /// since everything from that line down belongs to Klipper.
    /// Returns modified content, or std::nullopt if the section already exists.
    std::optional<std::string> add_section(const std::string& content,
                                           const std::string& section) const;

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

    /// Create backup of a config file at config_backup_path()
    void backup_file(IMoonrakerAPI& api, const std::string& file_path, SuccessCallback on_success,
                     ErrorCallback on_error);

    /// Copy every config_backup_path() backup back over its file
    void restore_backups(IMoonrakerAPI& api, SuccessCallback on_complete, ErrorCallback on_error);

    /// Delete every config_backup_path() backup (cleanup after successful edit)
    void cleanup_backups(IMoonrakerAPI& api, SuccessCallback on_complete);

    /// Get cached section map from last load_config_files() call
    std::map<std::string, SectionLocation> get_section_map() const;

    /// Get cached file content by path
    std::optional<std::string> get_cached_file(const std::string& path) const;

    /// Where the editor reads the config_edit feature's state. The default
    /// reads PrinterState, which only the main thread may do: from any other
    /// thread it answers Absent.
    void set_feature_state_source(FeatureStateSource source);

    /// How often the FIRMWARE_RESTART health monitor polls Klippy's state and
    /// the cancel flag. A monitor already running keeps the interval it started with.
    void set_restart_poll_interval(std::chrono::milliseconds interval) {
        restart_poll_interval_ = interval;
    }

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
    /// Success means Klippy came back READY on the new config and configfile.settings
    /// holds every @p expected value; anything else (an ERROR, no READY in time, an
    /// expected value that did not take) restores the backups, restarts again and
    /// reports why through @p on_error.
    /// With the plugin's config_edit feature ready, the plugin writes the edits
    /// instead (config_edit_route()), and the restart and revert are the same.
    /// Call safe_multi_edit() and safe_edit_value() from the main thread.
    void safe_multi_edit(IMoonrakerAPI& api, const std::string& section,
                         const std::vector<ConfigEdit>& edits, SuccessCallback on_success,
                         ErrorCallback on_error, int restart_timeout_ms = 15000,
                         std::vector<ExpectedSetting> expected = {});

  private:
    /// How a restart that Klipper does not survive is undone, and what a
    /// restart it survives cleans up.
    struct RestartRecovery {
        std::function<void(IMoonrakerAPI&, SuccessCallback, ErrorCallback)> restore;
        std::function<void(IMoonrakerAPI&, SuccessCallback)> cleanup;
    };

    /// The file-API route: load, apply, back up, upload, then restart.
    void file_api_multi_edit(IMoonrakerAPI& api, const std::string& section,
                             const std::vector<ConfigEdit>& edits, SuccessCallback on_success,
                             ErrorCallback on_error, int restart_timeout_ms,
                             std::vector<ExpectedSetting> expected);

    /// The plugin route: read the section, write through
    /// server.helix.config_edit, then restart. Falls back to the file route
    /// when the plugin cannot serve the list.
    void plugin_multi_edit(IMoonrakerAPI& api, const std::string& section,
                           const std::vector<ConfigEdit>& edits, SuccessCallback on_success,
                           ErrorCallback on_error, int restart_timeout_ms,
                           std::vector<ExpectedSetting> expected);

    /// Send FIRMWARE_RESTART and watch Klipper come back (judge_restart_poll());
    /// @p recovery undoes the edit when it does not come back READY with every
    /// @p expected setting within @p restart_timeout_ms.
    void restart_and_monitor(IMoonrakerAPI& api, RestartRecovery recovery,
                             SuccessCallback on_success, ErrorCallback on_error,
                             int restart_timeout_ms, std::vector<ExpectedSetting> expected);

    ConfigEditRoute route() const;

    /// Undo an edit whose restart failed: restore through @p recovery, restart
    /// again, and report @p problem with what the undo did. Any thread.
    static void revert_after_failed_restart(IMoonrakerAPI& api, const RestartRecovery& recovery,
                                            ErrorCallback on_error, const std::string& problem);

    /// Undo by copying each .helix_backup over its file; clean up by deleting them.
    RestartRecovery file_api_recovery();

    /// Copy each (backup, file) pair the plugin reported back over its file.
    static void
    restore_plugin_backups(IMoonrakerAPI& api,
                           const std::vector<std::pair<std::string, std::string>>& backups,
                           SuccessCallback on_complete, ErrorCallback on_error);

    FeatureStateSource feature_state_source_;

    std::chrono::milliseconds restart_poll_interval_{500};

    /// Remember a freshly submitted health monitor so the destructor can wait
    /// for it, and drop the futures of monitors that have already finished.
    void track_restart_monitor(std::future<void> monitor);

    /// Raise the cancel flag every live monitor polls, then wait (bounded) for
    /// them to unwind. Called from the destructor.
    void cancel_restart_monitors();

    /// Raised when the editor is going away. Held by shared_ptr and captured BY
    /// VALUE by each monitor, so the flag itself outlives the editor even if a
    /// wait times out and a monitor is still between polls.
    std::shared_ptr<std::atomic<bool>> monitor_cancel_ = std::make_shared<std::atomic<bool>>(false);

    /// Futures of health monitors submitted to HttpExecutor::fast().
    std::vector<std::future<void>> restart_monitors_;

    /// Protects restart_monitors_ — monitors are armed from Moonraker callbacks
    /// on background threads, while the destructor runs on the main thread.
    std::mutex monitor_mutex_;

    /// Cached section map from last load_config_files()
    std::map<std::string, SectionLocation> section_map_;

    /// Cached file contents from last load
    std::map<std::string, std::string> file_cache_;

    /// Protects section_map_ and file_cache_
    mutable std::mutex cache_mutex_;
};

} // namespace helix::system
