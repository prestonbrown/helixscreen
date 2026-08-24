// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "filament_sensor_types.h"
#include "json_fwd.h"
#include "lvgl.h"
#include "sensor_registry.h"
#include "subject_managed_panel.h"

// Forward declarations — IMoonrakerAPI is a GLOBAL-scope interface (see
// i_moonraker_api.h); declaring it inside namespace helix would create a
// distinct phantom type.
class IMoonrakerAPI;

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Test-only friend (defined in tests/unit/test_runout_empty_lane_scope.cpp) used
// to set per-sensor roles directly, bypassing the single-RUNOUT exclusivity in
// set_sensor_role() so multi-lane (Snapmaker) runout scenarios can be exercised.
class RunoutScopeTestAccess;
class BypassArmingTestAccess;
// Test-only friend (defined in tests/test_helpers/post_unload_grace_test_access.h)
// used to put the singleton on a known sensor baseline with the startup grace
// already expired.
class PostUnloadGraceTestAccess;

class AmsBackend;

namespace helix {

/**
 * @brief Central manager for filament sensor discovery, configuration, and state.
 *
 * Provides:
 * - Auto-discovery of sensors from Klipper objects list
 * - User configuration (role assignment, enable/disable)
 * - Real-time state tracking from Moonraker updates
 * - LVGL subjects for reactive UI binding
 * - Config persistence to settings.json
 *
 * Thread-safe for state updates from Moonraker callbacks.
 *
 * @code
 * // Initialize after Moonraker connection
 * auto& mgr = FilamentSensorManager::instance();
 * mgr.init_subjects();
 * mgr.discover_sensors(capabilities.get_filament_sensor_names());
 * mgr.load_config();
 *
 * // Check sensor state
 * if (mgr.is_filament_detected(FilamentSensorRole::RUNOUT)) {
 *     // Filament present
 * }
 * @endcode
 */
class FilamentSensorManager : public helix::sensors::ISensorManager {
  public:
    /// @brief Callback for sensor state change notifications
    using StateChangeCallback =
        std::function<void(const std::string& sensor_name, const FilamentSensorState& old_state,
                           const FilamentSensorState& new_state)>;

    /**
     * @brief Get singleton instance
     */
    static FilamentSensorManager& instance();

    // Prevent copying
    FilamentSensorManager(const FilamentSensorManager&) = delete;
    FilamentSensorManager& operator=(const FilamentSensorManager&) = delete;

    // ========================================================================
    // ISensorManager Interface
    // ========================================================================

    /// @brief Get category name for registry
    [[nodiscard]] std::string category_name() const override;

    /**
     * @brief Discover sensors from Klipper objects list
     *
     * Implements ISensorManager interface. Delegates to discover_sensors().
     * @note MUST be called from main LVGL thread (updates subjects directly)
     */
    void discover(const std::vector<std::string>& klipper_objects) override;

    // update_from_status() is declared below in "State Updates" section

    /**
     * @brief Load sensor configuration from JSON
     *
     * Note: This manager uses legacy Config-based persistence. This method
     * accepts JSON for ISensorManager compatibility but delegates to the
     * internal load_config_from_file() which reads from settings.json.
     *
     * @param config JSON config (currently ignored - uses internal config)
     * @note MUST be called from main LVGL thread (updates subjects directly)
     */
    void load_config(const nlohmann::json& config) override;

    /// @brief Save configuration to JSON
    [[nodiscard]] nlohmann::json save_config() const override;

    // ========================================================================
    // Initialization
    // ========================================================================

    /**
     * @brief Initialize LVGL subjects for UI binding
     *
     * Must be called before creating any XML components that bind to sensor subjects.
     * Safe to call multiple times (idempotent).
     */
    void init_subjects();

    /**
     * @brief Deinitialize LVGL subjects
     *
     * Must be called before lv_deinit() to properly disconnect observers.
     * Called by StaticSubjectRegistry during application shutdown.
     */
    void deinit_subjects();

    /**
     * @brief Discover sensors from PrinterCapabilities
     *
     * Populates internal sensor list from Klipper objects.
     * Should be called after Moonraker connection established.
     *
     * @param klipper_sensor_names Full Klipper object names from PrinterCapabilities
     */
    void discover_sensors(const std::vector<std::string>& klipper_sensor_names);

    /**
     * @brief Check if any sensors have been discovered
     */
    [[nodiscard]] bool has_sensors() const;

    /**
     * @brief Get all discovered sensor configurations (thread-safe copy)
     */
    [[nodiscard]] std::vector<FilamentSensorConfig> get_sensors() const;

    /**
     * @brief Get sensor count
     */
    [[nodiscard]] size_t sensor_count() const;

    // ========================================================================
    // Configuration
    // ========================================================================

    /**
     * @brief Load configuration from settings.json
     *
     * Merges saved config with discovered sensors. New sensors get default config,
     * removed sensors are preserved in config (in case they come back).
     *
     * @note This is the legacy config API. Use ISensorManager::load_config(json) for
     *       SensorRegistry integration.
     */
    void load_config_from_file();

    /**
     * @brief Save current configuration to settings.json
     *
     * @note This is the legacy config API. ISensorManager::save_config() returns
     *       JSON but also saves to file for this manager.
     */
    void save_config_to_file();

    /**
     * @brief Set role for a specific sensor
     *
     * @param klipper_name Full Klipper object name
     * @param role New role assignment
     */
    void set_sensor_role(const std::string& klipper_name, FilamentSensorRole role);

    /**
     * @brief Enable or disable a specific sensor
     *
     * @param klipper_name Full Klipper object name
     * @param enabled Whether sensor should be monitored
     */
    void set_sensor_enabled(const std::string& klipper_name, bool enabled);

    /**
     * @brief Set master enable switch
     *
     * When disabled, all sensor monitoring is bypassed.
     *
     * @param enabled Master enable state
     */
    void set_master_enabled(bool enabled);

    // ========================================================================
    // Bypass runout arming
    // ========================================================================

    /**
     * @brief Arm RUNOUT-role sensors at the Klipper firmware level for a bypass print.
     *
     * Universal bypass companion, independent of AMS backend: filament fed
     * through the bypass reaches the toolhead through no backend lane, so
     * mid-print runout protection falls to the toolhead sensor alone. On
     * firmwares that leave that sensor disabled outside their own filament
     * system (e.g. Creality's macros toggle it around every CFS operation),
     * a bypass print would otherwise run with no runout protection at all.
     *
     * Sends `SET_FILAMENT_SENSOR SENSOR=<name> ENABLE=1` for every RUNOUT-role
     * sensor that is present in Klipper and currently DISABLED at the firmware
     * level, recording what it armed. Idempotent: already-enabled or already
     * armed sensors are skipped. The HelixScreen-side enabled flag (user
     * config) is deliberately untouched — arming is a temporary firmware-state
     * change, not a settings change.
     *
     * @param api API handle for the gcode send (may be null — no-op then)
     * @return number of sensors newly armed
     */
    int arm_runout_sensors_for_bypass(class IMoonrakerAPI* api);

    /**
     * @brief Restore the sensors armed by arm_runout_sensors_for_bypass().
     *
     * Sends `SET_FILAMENT_SENSOR SENSOR=<name> ENABLE=0` for exactly the
     * sensors this manager armed (and that still exist in our config),
     * returning them to the pre-bypass firmware state. Sensors disabled by
     * someone else in the meantime are left alone — the armed set is the only
     * authority we have.
     *
     * @param api API handle (may be null — the armed set is kept for a later retry)
     * @return number of sensors restored
     */
    int restore_runout_sensors_after_bypass(class IMoonrakerAPI* api);

    /**
     * @brief Whether any sensor is currently held armed by bypass
     */
    [[nodiscard]] bool has_bypass_armed_sensors() const;

    /**
     * @brief Bypass-transition entry point — arms on engage, restores on disengage.
     *
     * The whole bypass⇄sensor policy lives in this manager (the sensor
     * abstraction layer); AMS code never implements it, it only reports the
     * transition. AmsState calls this from its any_bypass_active() edge.
     *
     * @param active new bypass state
     */
    void on_bypass_active_changed(bool active);

    /**
     * @brief Supply the API handle used for bypass arming sends
     *
     * Wired by MoonrakerManager alongside AmsState::set_moonraker_api().
     * May be null (mocks, early shutdown) — arming is a no-op then.
     */
    void set_moonraker_api(class IMoonrakerAPI* api);

    /**
     * @brief Check if master switch is enabled
     */
    [[nodiscard]] bool is_master_enabled() const;

    // ========================================================================
    // State Queries
    // ========================================================================

    /**
     * @brief Check if filament is detected for a given role
     *
     * Returns false if master disabled, sensor disabled, or no sensor assigned to role.
     *
     * @param role The sensor role to check
     * @return true if filament is detected
     */
    [[nodiscard]] bool is_filament_detected(FilamentSensorRole role) const;

    /**
     * @brief Check if a sensor is available (exists and enabled)
     *
     * @param role The sensor role to check
     * @return true if sensor exists, is enabled, and is available in Klipper
     */
    [[nodiscard]] bool is_sensor_available(FilamentSensorRole role) const;

    /**
     * @brief Get current state for a sensor by role (thread-safe copy)
     *
     * @param role The sensor role to query
     * @return State copy if sensor assigned to role, empty optional otherwise
     */
    [[nodiscard]] std::optional<FilamentSensorState>
    get_sensor_state(FilamentSensorRole role) const;

    /**
     * @brief Check if any sensor reports runout (no filament)
     *
     * Only checks enabled sensors with assigned roles.
     *
     * @return true if any monitored sensor shows no filament
     */
    [[nodiscard]] bool has_any_runout() const;

    /**
     * @brief Check for a GENUINE runout, ignoring empty / never-loaded lanes.
     *
     * Like has_any_runout(), but for a sensor that maps to an AMS lane
     * (Snapmaker "e{N}_filament" -> slot N) it consults the active AMS backend
     * and treats a no-filament reading on a lane the backend reports as EMPTY /
     * not-present as a non-event, NOT a runout. A lane deliberately left
     * unloaded for a multi-color print must not raise the idle runout modal.
     *
     * A sensor that DOES NOT map to a lane (no AMS backend, or an unmappable
     * sensor name) is treated exactly as has_any_runout() would — its
     * no-filament reading still counts, so a plain single-extruder runout
     * sensor keeps working. A lane that was LOADED and lost filament mid-use
     * (slot present, sensor now false) still reports a real runout.
     *
     * @return true if at least one runout-role sensor reports no filament on a
     *         lane that is loaded/present (or that does not map to a lane).
     */
    [[nodiscard]] bool has_real_runout() const;

    /**
     * @brief Find required tools whose mapped AMS lane is genuinely empty.
     *
     * Scoped, lane-truth pre-print/in-print emptiness check. For each tool in
     * @p tools_used, resolves the AMS slot it routes to (via @p remap, falling
     * back to the firmware-default head = clamp(tool, 0..3)) and consults the
     * active AMS backend's authoritative per-slot presence
     * (get_slot_info(slot).is_present(), which on Snapmaker U1 reflects
     * print_task_config.filament_exist[head]). This is deliberately NOT the
     * per-tool motion sensor, which reads filament_detected=false whenever
     * filament is staged in the lane but retracted from the toolhead.
     *
     * A tool is flagged ONLY when its lane is genuinely not present. Tools whose
     * lane is present (even if their toolhead motion sensor reads false) are not
     * flagged. Unused heads (not in @p tools_used) are never considered.
     *
     * Returns {} when there is no AMS backend (no lane truth to consult — the
     * caller should fall back to the aggregate sensor check), when master/sensor
     * runout protection is disabled, or when @p tools_used is empty.
     *
     * @param tools_used Logical tools the print actually uses.
     * @param remap      Tool→slot remap the print will use (identity if empty).
     * @return (tool_index, slot_index) pairs for required tools with empty lanes.
     */
    [[nodiscard]] std::vector<std::pair<int, int>>
    find_empty_required_lanes(const std::set<int>& tools_used,
                              const std::map<int, int>& remap) const;

    /**
     * @brief Compute the runout-badge encoding scoped to the active print.
     *
     * Returns the same -1/0/1/2 encoding the unscoped filament_runout_detected
     * subject uses, but considering ONLY the tools in @p tools_used and using AMS
     * lane truth (see find_empty_required_lanes):
     *   -1 = no runout-role sensor configured, or @p tools_used is empty (no
     *        opinion — badge hidden)
     *    0 = at least one required tool's lane is genuinely empty (runout / red)
     *    1 = every required tool's lane is present (loaded / green)
     *    2 = runout protection disabled (master or sensor — muted)
     *
     * With no AMS backend there is no lane truth, so this falls back to the
     * aggregate runout-role value (the unscoped behavior), keeping single-
     * extruder / non-AMS printers unchanged.
     *
     * @param tools_used Logical tools the print actually uses.
     * @param remap      Tool→slot remap the print will use (identity if empty).
     */
    [[nodiscard]] int compute_scoped_runout_value(const std::set<int>& tools_used,
                                                  const std::map<int, int>& remap) const;

    /**
     * @brief Check if motion sensor encoder is active
     *
     * Only applicable for motion sensors during extrusion.
     *
     * @return true if motion sensor shows encoder activity
     */
    [[nodiscard]] bool is_motion_active() const;

    /**
     * @brief Check if Z probe is triggered
     *
     * Returns false if master disabled, probe sensor disabled, or no probe assigned.
     *
     * @return true if Z probe sensor is triggered
     */
    [[nodiscard]] bool is_probe_triggered() const;

    // ========================================================================
    // State Updates
    // ========================================================================

    /**
     * @brief Update sensor states from Moonraker notification
     *
     * Called by PrinterState when receiving notify_status_update.
     * Implements ISensorManager interface.
     * Thread-safe.
     *
     * @param status JSON object containing sensor state updates
     */
    void update_from_status(const nlohmann::json& status) override;

    /**
     * @brief Register callback for state changes
     *
     * @param callback Function to call when any sensor state changes
     */
    void set_state_change_callback(StateChangeCallback callback);

    // ========================================================================
    // LVGL Subjects
    // ========================================================================

    /**
     * @brief Get subject for runout sensor detected state
     * @return Subject (int: 0=no filament, 1=detected, -1=no sensor)
     */
    [[nodiscard]] lv_subject_t* get_runout_detected_subject();

    /**
     * @brief Get subject for the print-scoped runout state (FIX B).
     *
     * Same -1/0/1/2 encoding as get_runout_detected_subject(), but scoped to the
     * active print's used tools using AMS lane truth. The in-print runout badge
     * binds THIS subject so a tool whose filament is staged in the lane (motion
     * sensor false) does not flag, and unused empty heads never flag. Drive it
     * from the active context via set_scoped_runout() — the unscoped subject
     * stays as-is for non-print contexts.
     *
     * @return Subject (int: -1=no opinion, 0=runout, 1=loaded, 2=disabled)
     */
    [[nodiscard]] lv_subject_t* get_scoped_runout_subject();

    /**
     * @brief Publish a print-scoped runout value into the scoped subject.
     *
     * Computed by the owner of "the active print's used tools" (PrintStatusPanel)
     * via compute_scoped_runout_value(). MUST be called on the main LVGL thread.
     *
     * @param value -1/0/1/2 (see get_scoped_runout_subject()).
     */
    void set_scoped_runout(int value);

    /**
     * @brief Get subject for toolhead sensor detected state
     * @return Subject (int: 0=no filament, 1=detected, -1=no sensor)
     */
    [[nodiscard]] lv_subject_t* get_toolhead_detected_subject();

    /**
     * @brief Get subject for entry sensor detected state
     * @return Subject (int: 0=no filament, 1=detected, -1=no sensor)
     */
    [[nodiscard]] lv_subject_t* get_entry_detected_subject();

    /**
     * @brief Get subject for Z probe triggered state
     * @return Subject (int: 0=not triggered, 1=triggered, -1=no sensor)
     */
    [[nodiscard]] lv_subject_t* get_probe_triggered_subject();

    /**
     * @brief Get subject for any runout active (any sensor shows no filament)
     * @return Subject (int: 0=all OK, 1=runout detected)
     */
    [[nodiscard]] lv_subject_t* get_any_runout_subject();

    /**
     * @brief Get subject for motion sensor activity
     * @return Subject (int: 0=idle, 1=motion detected)
     */
    [[nodiscard]] lv_subject_t* get_motion_active_subject();

    /**
     * @brief Get subject for master enable state
     * @return Subject (int: 0=disabled, 1=enabled)
     */
    [[nodiscard]] lv_subject_t* get_master_enabled_subject();

    /**
     * @brief Get subject for sensor count (for conditional UI visibility)
     * @return Subject (int: number of discovered sensors)
     */
    [[nodiscard]] lv_subject_t* get_sensor_count_subject();

    /**
     * @brief Check if still within sensor stabilization grace period
     *
     * Used to suppress notifications and modals while sensor states
     * are being synchronized after Moonraker connection.
     *
     * @return true if within grace period (first 2 seconds after sensor discovery)
     */
    [[nodiscard]] bool is_in_startup_grace_period() const;

    /**
     * @brief Enable synchronous mode for testing
     *
     * When enabled, update_from_status() calls update_subjects() synchronously
     * instead of using lv_async_call(). This avoids LVGL timer dependencies in unit tests.
     *
     * @param enabled true to enable synchronous updates
     */
    void set_sync_mode(bool enabled);

    /**
     * @brief Update subjects on main LVGL thread (called by async callback)
     *
     * This is public to allow the async callback in the anonymous namespace
     * to access it. Do not call directly - use update_from_status() instead.
     */
    void update_subjects_on_main_thread();

  private:
    friend class FilamentSensorManagerTestAccess;
    friend class ::RunoutScopeTestAccess;
    friend class ::BypassArmingTestAccess;
    friend class ::PostUnloadGraceTestAccess;

    FilamentSensorManager();
    ~FilamentSensorManager();

    /**
     * @brief Extract sensor name and type from Klipper object name
     *
     * @param klipper_name Full name like "filament_switch_sensor fsensor"
     * @param[out] sensor_name Extracted short name
     * @param[out] type Detected sensor type
     * @return true if successfully parsed
     */
    bool parse_klipper_name(const std::string& klipper_name, std::string& sensor_name,
                            FilamentSensorType& type) const;

    /**
     * @brief Find config by Klipper name
     * @return Pointer to config, or nullptr if not found
     */
    FilamentSensorConfig* find_config(const std::string& klipper_name);
    const FilamentSensorConfig* find_config(const std::string& klipper_name) const;

    /**
     * @brief Find config by assigned role
     * @return Pointer to config, or nullptr if no sensor has this role
     */
    const FilamentSensorConfig* find_config_by_role(FilamentSensorRole role) const;

    /// Result of one scoped lane scan — shared by find_empty_required_lanes()
    /// and compute_scoped_runout_value() (dedups config lookup + backend fetch +
    /// availability gate + per-lane scan).
    struct ScopedRunoutScan {
        std::vector<std::pair<int, int>> empty_lanes; ///< (tool, slot) genuinely empty
        ::AmsBackend* backend = nullptr;              ///< active backend (nullptr -> no lane truth)
        bool no_used_tools = false;                   ///< tools_used was empty
        bool runout_configured = false;               ///< a RUNOUT-role sensor exists
        bool runout_enabled = false;                  ///< master && sensor enabled
        bool sensor_available = false;                ///< fresh status received (freshness gate)
        bool aggregate_detected = false;              ///< no-backend fallback sensor reading
    };

    /// Shared scoped lane scan. Caller MUST hold mutex_ (recursive).
    [[nodiscard]] ScopedRunoutScan scan_required_lanes(const std::set<int>& tools_used,
                                                       const std::map<int, int>& remap) const;

    /**
     * @brief Update all LVGL subjects from current state
     */
    void update_subjects();

    // Recursive mutex for thread-safe state access
    // Recursive because update_subjects() calls has_any_runout()/is_motion_active()
    mutable std::recursive_mutex mutex_;

    // Configuration
    bool master_enabled_ = true;
    std::vector<FilamentSensorConfig> sensors_;

    /// Klipper names of sensors WE armed for bypass (restore set). Empty when
    /// no bypass arming is outstanding.
    std::vector<std::string> bypass_armed_;

    /// API handle for bypass arming sends (not owned). Set by
    /// MoonrakerManager; may be null.
    class IMoonrakerAPI* api_ = nullptr;

    /// One SET_FILAMENT_SENSOR send — shared by arm and restore so the
    /// command shape and error disposition stay identical.
    void send_firmware_sensor_enable(class IMoonrakerAPI* api, const FilamentSensorConfig& sensor,
                                     bool enabled);

    // Runtime state (keyed by klipper_name)
    std::map<std::string, FilamentSensorState> states_;

    // State change callback
    StateChangeCallback state_change_callback_;

    // Test mode: when true, update_from_status() calls update_subjects() synchronously
    // instead of using lv_async_call(). This avoids LVGL timer dependencies in unit tests.
    bool sync_mode_ = false;

    // Tracks whether we've received the first status update from Moonraker.
    // Ensures update_subjects() fires on initial status even when sensor state
    // matches defaults (filament_detected=false), which wouldn't trigger any_changed.
    bool initial_status_received_ = false;

    // Discovery time for suppressing initial state notifications
    // Reset when sensors are discovered (after Moonraker connects), not at app startup
    std::chrono::steady_clock::time_point startup_time_;

    // LVGL subjects
    bool subjects_initialized_ = false;
    SubjectManager subjects_;
    lv_subject_t runout_detected_;
    lv_subject_t scoped_runout_; ///< Print-scoped runout (FIX B); driven by PrintStatusPanel
    lv_subject_t toolhead_detected_;
    lv_subject_t entry_detected_;
    lv_subject_t probe_triggered_;
    lv_subject_t any_runout_;
    lv_subject_t motion_active_;
    lv_subject_t master_enabled_subject_;
    lv_subject_t sensor_count_;
};

} // namespace helix
