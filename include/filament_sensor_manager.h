// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "filament_sensor_types.h"
#include "json_fwd.h"
#include "sensor_manager.h"

// Forward declarations
namespace helix {
class IMoonrakerAPI;
}

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace helix {

// Test-only friend (defined in tests/unit/test_runout_empty_lane_scope.cpp) used
// to set per-sensor roles directly, the way a settings.json restore does, without
// going through set_role().
class RunoutScopeTestAccess;
class BypassArmingTestAccess;
// Test-only friend (defined in tests/test_helpers/post_unload_grace_test_access.h)
// used to put the singleton on a known sensor baseline with the startup grace
// already expired.
class PostUnloadGraceTestAccess;

class AmsBackend;

enum class PrintJobState;

/**
 * @brief Does the current print state give the scoped runout badge anything to say?
 *
 * True only for PRINTING and PAUSED. Deliberately NARROWER than
 * PrintLifecycleState::is_active(), which also counts Preparing: the badge is
 * scoped to the tools the RUNNING file uses, and during a preparing window the
 * panel's get_tools_used() still describes the PREVIOUS job — widening this
 * would scope the badge to the wrong file instead of hiding it.
 *
 * A free function rather than an inline guard in PrintStatusPanel because the
 * badge's terminal-state behaviour is the whole of issue 9 and needs to be
 * assertable without building a laid-out panel and a parsed gcode viewer.
 */
[[nodiscard]] bool print_scopes_runout_badge(PrintJobState state);

/**
 * @brief Filament sensors (filament_switch_sensor, filament_motion_sensor):
 *        runout, presence, and the firmware-level arming around a bypass.
 *
 * Discovery, the entries, enabled state, persistence under "filament_sensors"
 * and publishing come from SensorManager. This class owns what is filament's
 * own: the per-sensor readings and lanes, the master switch, the runout and
 * presence queries, lane scoping against the active AMS backend, the startup
 * grace period, removal-dwell toasts, the state-change callback and bypass
 * arming.
 *
 * Two base behaviors are overridden. update_from_status() reads AmsState
 * BEFORE taking the mutex (AmsState calls into this class from under its own
 * lock, so the other order is an ABBA cycle) and fires toasts and the
 * state-change callback after releasing it, on the calling thread.
 * set_role() makes RUNOUT exclusive per lane (lane_index_for_sensor) rather
 * than per printer.
 *
 * A name containing "runout" starts as RUNOUT when no sensor holds it. Saved
 * settings carry the lane of a sensor that has one and the master switch.
 */
class FilamentSensorManager : public sensors::SensorManager {
  public:
    /// @brief Callback for sensor state change notifications
    using StateChangeCallback =
        std::function<void(const std::string& sensor_name, const FilamentSensorState& old_state,
                           const FilamentSensorState& new_state)>;

    /**
     * @brief Get singleton instance
     */
    static FilamentSensorManager& instance();

    /// Typed view of the entries, with each sensor's configured lane.
    [[nodiscard]] std::vector<FilamentSensorConfig> get_sensors() const;

    /**
     * @brief Give @p role to a sensor.
     *
     * A role has one holder, except RUNOUT, which has one per lane: assigning
     * it clears it from any other sensor watching the same lane, and from every
     * sensor watching none.
     */
    void set_role(std::string_view klipper_name, uint8_t role) override;

    /// Settings lists standalone sensors only: an AMS backend owns the rest.
    [[nodiscard]] bool listed_in_settings(const sensors::SensorEntry& entry) const override;
    /// The Enabled switch shows only for a sensor that holds a role.
    [[nodiscard]] sensors::SensorRowView row(size_t index) const override;

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
     * Presence query, not a runout decision: a sensor the firmware stood down
     * (SET_FILAMENT_SENSOR ENABLE=0) still reports and is still read. With
     * several holders of the role the firmware-running one is preferred, then
     * the first holder. Runout alerting is gated separately (monitors_runout).
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
     * Same presence split as is_filament_detected: a firmware stand-down does
     * not make the sensor unavailable.
     *
     * @param role The sensor role to check
     * @return true if sensor exists, is enabled, and is available in Klipper
     */
    [[nodiscard]] bool is_sensor_available(FilamentSensorRole role) const;

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
     * When the backend names the lane being fed (get_current_slot() >= 0), only
     * the sensor of that lane, or of the current tool, can report a runout: the
     * other lanes are idle, and a lane whose filament waits in a feeder short of
     * the toolhead reads empty at its own switch. The tool match keeps a
     * per-toolhead sensor (lane = tool index) live on a multi-extruder system
     * whose current slot counts lanes. With no current lane, every present
     * lane counts.
     *
     * @return true if at least one runout-role sensor reports no filament on a
     *         lane that is loaded/present (or that does not map to a lane).
     */
    [[nodiscard]] bool has_real_runout() const;

    /**
     * @brief The lane of the runout has_real_runout() finds, or nullopt for none.
     *
     * -1 when the runout is real but its sensor maps to no lane (a plain
     * toolhead or extruder sensor).
     */
    [[nodiscard]] std::optional<int> real_runout_lane() const;

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
     * @brief The motion sensor that watches for jams, for the clog meter
     *
     * Only motion sensors Settings lists (not AMS-owned), that the user has
     * enabled (with the master switch on) and that Klipper reports are
     * candidates. Among them: the one whose lane is
     * @p active_lane, else the first armed one reporting no movement (the worst
     * reading), else the first one Klipper has armed, else the first.
     *
     * @param active_lane Loaded slot or tool, -1 when none
     * @param filament_hardware_absent The filament system reported no hardware
     *        behind it (AmsSystemInfo::external_spool_only); passed in because
     *        AmsState calls this from under its own lock
     * @return nullopt when no motion sensor qualifies
     */
    [[nodiscard]] std::optional<ClogMotionSensor>
    clog_motion_sensor(int active_lane, bool filament_hardware_absent) const;

    // ========================================================================
    // State Updates
    // ========================================================================

    /**
     * @brief Update sensor states from a Moonraker status frame
     *
     * Any thread. Parses under the mutex, publishes, then fires the
     * state-change callback and toasts with the mutex released.
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
     * @brief Publish a print-scoped runout value into the scoped subject.
     *
     * Computed by the owner of "the active print's used tools" (PrintStatusPanel)
     * via compute_scoped_runout_value(). MUST be called on the main LVGL thread.
     *
     * @param value -1/0/1/2: no opinion, runout, loaded, disabled. The
     *        filament_runout_scoped subject; the in-print badge binds it.
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
     * @brief Get subject for any runout active (any sensor shows no filament)
     * @return Subject (int: 0=all OK, 1=runout detected)
     */
    [[nodiscard]] lv_subject_t* get_any_runout_subject();

    /// Changes whenever anything clog_motion_sensor() reads changes: a motion
    /// sensor's user enable, availability, armed state or reading, or the
    /// master switch. The value itself means nothing.
    [[nodiscard]] lv_subject_t* get_motion_signature_subject();

    /// Ticks when a sensor's role or enabled flag, or the master switch, changes.
    /// A reading that moves because of configuration is not a physical event.
    [[nodiscard]] lv_subject_t* get_config_revision_subject();

    /**
     * @brief Check if still within sensor stabilization grace period
     *
     * Used to suppress notifications and modals while sensor states
     * are being synchronized after Moonraker connection.
     *
     * @return true within app_constants::startup::kSensorStabilizationPeriod of sensor discovery
     */
    [[nodiscard]] bool is_in_startup_grace_period() const;

  protected:
    void register_subjects(SubjectManager& subjects) override;
    void refresh_subjects() override;
    /// Unused: update_from_status() is overridden and parses inline.
    bool parse_status(const nlohmann::json& status) override;
    void on_discovered(const PrinterDiscovery& hw) override;
    void on_entries_changed() override;
    void read_entry_extra(sensors::SensorEntry& entry, const nlohmann::json& saved) override;
    void write_entry_extra(const sensors::SensorEntry& entry, nlohmann::json& out) const override;
    void read_extra_settings(const nlohmann::json& saved) override;
    void write_extra_settings(nlohmann::json& out) const override;

  private:
    friend class FilamentSensorManagerTestAccess;
    friend class RunoutScopeTestAccess;
    friend class BypassArmingTestAccess;
    friend class PostUnloadGraceTestAccess;

    FilamentSensorManager();
    static Options make_options();

    /// The AMS slot a sensor watches, or -1 when it watches none (a
    /// single-extruder runout sensor), in which case the caller must NOT
    /// lane-scope it. Its saved "lane" wins; otherwise the Snapmaker U1 name
    /// "e<N>_filament" (motion or switch form) encodes slot N. Caller holds
    /// the mutex.
    [[nodiscard]] int lane_index_for_sensor(const sensors::SensorEntry& sensor) const;

    /// The reading for @p klipper_name, or nullptr when none is kept. Caller
    /// holds the mutex.
    [[nodiscard]] FilamentSensorState* state(const std::string& klipper_name);
    [[nodiscard]] const FilamentSensorState* state(const std::string& klipper_name) const;

    /**
     * @brief Whether this sensor's reading counts for runout/presence decisions
     *
     * One rule shared by every runout consumer: the user's config enables the
     * sensor, it holds a role, and the firmware is running it. Klipper keeps
     * reporting filament_detected for a sensor stood down with
     * SET_FILAMENT_SENSOR ENABLE=0 but takes no runout action of its own, so
     * neither do we. The state default is enabled=true, so a sensor that has
     * never reported the field counts as running. A runout observed while the
     * firmware ran the sensor keeps counting after a stand-down (see
     * observed_runouts_). Caller MUST hold the mutex.
     */
    [[nodiscard]] bool monitors_runout(const sensors::SensorEntry& sensor) const;

    /**
     * @brief First holder of @p role the firmware is running
     *
     * When every holder of the role is stood down, the first holder is
     * returned anyway, so a caller can still tell "configured but not
     * running" from "no sensor holds this role". Caller MUST hold the mutex.
     */
    [[nodiscard]] const sensors::SensorEntry*
    find_monitoring_config_by_role(FilamentSensorRole role) const;

    /// Result of one scoped lane scan — shared by find_empty_required_lanes()
    /// and compute_scoped_runout_value() (dedups config lookup + backend fetch +
    /// availability gate + per-lane scan).
    struct ScopedRunoutScan {
        std::vector<std::pair<int, int>> empty_lanes; ///< (tool, slot) genuinely empty
        AmsBackend* backend = nullptr;                ///< active backend (nullptr -> no lane truth)
        bool no_used_tools = false;                   ///< tools_used was empty
        bool runout_configured = false;               ///< a RUNOUT-role sensor exists
        bool runout_enabled = false;                  ///< master && sensor enabled
        bool sensor_available = false;                ///< fresh status received (freshness gate)
        bool aggregate_detected = false;              ///< no-backend fallback sensor reading
    };

    /// Shared scoped lane scan. Caller MUST hold the mutex (recursive).
    /// @p read_stood_down: whether a head sensor the firmware has stood down
    /// still counts as that head's reading. The pre-print check needs it (the
    /// firmware holds every head down between prints); the running-print badge
    /// does not (a stood-down head is one the job is not feeding from).
    [[nodiscard]] ScopedRunoutScan scan_required_lanes(const std::set<int>& tools_used,
                                                       const std::map<int, int>& remap,
                                                       bool read_stood_down) const;

    /// Fingerprint of every input clog_motion_sensor() reads. Caller holds the mutex.
    [[nodiscard]] int motion_signature() const;
    /// Fingerprint of every sensor's role and enabled flag and the master
    /// switch: config_revision ticks when it moves. Caller holds the mutex.
    [[nodiscard]] uint32_t config_signature() const;

    bool master_enabled_ = true;
    /// Readings by klipper_name. A sensor missing from a later discovery keeps
    /// its reading, marked unavailable (on_entries_changed).
    std::map<std::string, FilamentSensorState> readings_;
    /// Saved "lane" by klipper_name; absent when the sensor has none.
    std::map<std::string, int> lanes_;
    /// Names present at the last discovery: only a name new to this set gets
    /// the RUNOUT name default, so a rediscovery never undoes the user's choice.
    std::set<std::string> known_;

    /// Klipper names of sensors WE armed for bypass (restore set). Empty when
    /// no bypass arming is outstanding.
    std::vector<std::string> bypass_armed_;

    /// API handle for bypass arming sends (not owned). Set by
    /// MoonrakerManager; may be null.
    class IMoonrakerAPI* api_ = nullptr;

    /// One SET_FILAMENT_SENSOR send — shared by arm and restore so the
    /// command shape and error disposition stay identical.
    void send_firmware_sensor_enable(class IMoonrakerAPI* api, const sensors::SensorEntry& sensor,
                                     bool enabled);

    /// Runout-role sensors whose removal toast is serving
    /// helix::kRunoutToastDwell, keyed by klipper_name and holding
    /// the moment the sensor went clear. Filament returning before the dwell
    /// expires drops the entry, so a tool change announces nothing at all.
    std::map<std::string, std::chrono::steady_clock::time_point> pending_removal_toast_;

    /// Sensors that went empty while the firmware ran them, mid-job and outside
    /// a filament operation, and have not refilled since. A pause macro that
    /// stands every sensor down still leaves this runout for the modal to
    /// report; a sensor the user keeps disabled never gets here. Cleared on
    /// refill and when the job lets go of the machine. Keyed by klipper_name.
    std::set<std::string> observed_runouts_;

    // State change callback
    StateChangeCallback state_change_callback_;

    // Tracks whether we've received the first status update from Moonraker.
    // Ensures the subjects refresh on initial status even when sensor state
    // matches defaults (filament_detected=true), which wouldn't trigger any_changed.
    bool initial_status_received_ = false;

    // Discovery time for suppressing initial state notifications
    // Reset when sensors are discovered (after Moonraker connects), not at app startup
    std::chrono::steady_clock::time_point startup_time_;

    // LVGL subjects (main thread)
    lv_subject_t runout_detected_{};
    lv_subject_t scoped_runout_{}; ///< Print-scoped runout; driven by PrintStatusPanel
    lv_subject_t toolhead_detected_{};
    lv_subject_t entry_detected_{};
    lv_subject_t any_runout_{};
    lv_subject_t motion_signature_{};
    lv_subject_t master_enabled_subject_{};
    lv_subject_t config_revision_{};
    /// config_signature() at the last refresh. Main thread only.
    std::optional<uint32_t> published_config_signature_;
};

} // namespace helix
