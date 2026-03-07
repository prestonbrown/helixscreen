// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "capability_overrides.h"
#include "hardware_validator.h"
#include "lvgl/lvgl.h"
#include "printer_calibration_state.h"
#include "printer_capabilities_state.h"
#include "printer_composite_visibility_state.h"
#include "printer_detector.h"
#include "printer_discovery.h"
#include "printer_excluded_objects_state.h"
#include "printer_fan_state.h"
#include "printer_hardware_validation_state.h"
#include "printer_led_state.h"
#include "printer_motion_state.h"
#include "printer_network_state.h"
#include "printer_plugin_status_state.h"
#include "printer_print_state.h"
#include "printer_temperature_state.h"
#include "printer_versions_state.h"
#include "spdlog/spdlog.h"
#include "state/subject_macros.h"
#include "subject_managed_panel.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "hv/json.hpp" // libhv's nlohmann json (via cpputil/)

namespace helix {

/**
 * @brief Network connection status states
 */
enum class NetworkStatus {
    DISCONNECTED, ///< No network connection
    CONNECTING,   ///< Connecting to network
    CONNECTED     ///< Connected to network
};

/**
 * @brief Printer connection status states
 */
enum class PrinterStatus {
    DISCONNECTED, ///< Printer not connected
    READY,        ///< Printer connected and ready
    PRINTING,     ///< Printer actively printing
    ERROR         ///< Printer in error state
};

/**
 * @brief Klipper firmware state (klippy_state from Moonraker)
 *
 * Represents the state of the Klipper firmware service, independent of
 * the Moonraker WebSocket connection. When klippy_state is not READY,
 * the printer cannot accept G-code commands even if Moonraker is connected.
 */
enum class KlippyState {
    READY = 0,    ///< Normal operation, printer ready for commands
    STARTUP = 1,  ///< Klipper is starting up (during RESTART/FIRMWARE_RESTART)
    SHUTDOWN = 2, ///< Emergency shutdown (M112)
    ERROR = 3     ///< Klipper error state (check klippy.log)
};

/**
 * @brief Print job state (from Moonraker print_stats.state)
 *
 * Represents the state of the current print job as reported by Klipper/Moonraker.
 * This is the canonical enum for print job state throughout HelixScreen.
 *
 * @note Values are chosen to match the integer representation used internally
 *       by MoonrakerClientMock for backward compatibility.
 */
enum class PrintJobState {
    STANDBY = 0,   ///< No active print, printer idle (Moonraker: "standby")
    PRINTING = 1,  ///< Actively printing (Moonraker: "printing")
    PAUSED = 2,    ///< Print paused (Moonraker: "paused")
    COMPLETE = 3,  ///< Print finished successfully (Moonraker: "complete")
    CANCELLED = 4, ///< Print cancelled by user (Moonraker: "cancelled")
    ERROR = 5      ///< Print failed with error (Moonraker: "error")
};

/**
 * @brief Terminal outcome of a print job (for UI persistence)
 *
 * Captures how the last print ended. Unlike PrintJobState (which always reflects
 * the current Moonraker state, including STANDBY after completion), PrintOutcome
 * persists the terminal state until a new print starts.
 *
 * This allows the UI to show "Print Complete!" or "Print Cancelled" badges and
 * Reprint buttons even after Moonraker transitions to STANDBY.
 *
 * @note NONE means either no print has occurred, or we're in the middle of a print.
 */
enum class PrintOutcome {
    NONE = 0,      ///< No completed print (printing, or never printed)
    COMPLETE = 1,  ///< Last print finished successfully
    CANCELLED = 2, ///< Last print was cancelled by user
    ERROR = 3      ///< Last print failed with error
};

/**
 * @brief Parse Moonraker print state string to PrintJobState enum
 *
 * Converts Moonraker's print_stats.state string to the corresponding enum.
 * Unknown strings default to STANDBY.
 *
 * @param state_str Moonraker state string (e.g., "printing", "paused")
 * @return Corresponding PrintJobState enum value
 */
PrintJobState parse_print_job_state(const char* state_str);

/**
 * @brief Print start initialization phase (detected from G-code response output)
 *
 * Represents the current phase during PRINT_START macro execution.
 * Used to show progress to the user during the initialization sequence
 * before actual printing begins.
 *
 * @note Phases are detected via best-effort pattern matching on G-code responses.
 *       Not all macros output all phases - progress estimation handles missing phases.
 */
enum class PrintStartPhase {
    IDLE = 0,           ///< Not in PRINT_START (normal operation)
    INITIALIZING = 1,   ///< PRINT_START detected, waiting for phases
    HOMING = 2,         ///< G28 / Home All Axes detected
    HEATING_BED = 3,    ///< M140/M190 / Heating bed detected
    HEATING_NOZZLE = 4, ///< M104/M109 / Heating nozzle detected
    QGL = 5,            ///< QUAD_GANTRY_LEVEL detected
    Z_TILT = 6,         ///< Z_TILT_ADJUST detected
    BED_MESH = 7,       ///< BED_MESH_CALIBRATE or BED_MESH_PROFILE LOAD detected
    CLEANING = 8,       ///< CLEAN_NOZZLE / nozzle wipe detected
    PURGING = 9,        ///< VORON_PURGE / LINE_PURGE detected
    COMPLETE = 10       ///< Transitioning to PRINTING state
};

/**
 * @brief Z-offset calibration strategy — determines gcode commands for calibration and save
 *
 * Different printers need different approaches to calibrate and persist Z-offset.
 * ForgeX-mod printers use SET_GCODE_OFFSET (auto-persisted by mod macro).
 * Standard Klipper uses PROBE_CALIBRATE -> ACCEPT -> SAVE_CONFIG.
 * Endstop printers use Z_ENDSTOP_CALIBRATE -> ACCEPT -> SAVE_CONFIG.
 */
enum class ZOffsetCalibrationStrategy {
    PROBE_CALIBRATE, ///< Standard Klipper: PROBE_CALIBRATE -> ACCEPT -> SAVE_CONFIG
    GCODE_OFFSET,    ///< ForgeX mod: G28 -> move -> G1 adjustments -> SET_GCODE_OFFSET
    ENDSTOP ///< Endstop: Z_ENDSTOP_CALIBRATE -> ACCEPT -> Z_OFFSET_APPLY_ENDSTOP -> SAVE_CONFIG
};

/**
 * @brief Convert PrintJobState enum to display string
 *
 * Returns a human-readable string for UI display.
 *
 * @param state PrintJobState enum value
 * @return Display string (e.g., "Printing", "Paused")
 */
const char* print_job_state_to_string(PrintJobState state);

/**
 * @brief Printer state manager with LVGL 9 reactive subjects
 *
 * Implements hybrid architecture:
 * - LVGL subjects for UI-bound data (automatic reactive updates)
 * - JSON cache for complex data (file lists, capabilities, metadata)
 *
 * @note Thread Safety: Public setters that update LVGL subjects (set_printer_capabilities,
 *       set_klipper_version, etc.) use lv_async_call internally to defer updates to the
 *       main thread. This allows safe calls from WebSocket callbacks without risking
 *       "Invalidate area not allowed during rendering" assertions.
 */
class PrinterState {
  public:
    /**
     * @brief Construct printer state manager
     *
     * Initializes internal data structures. Call init_subjects() before
     * creating XML components.
     */
    PrinterState();

    /**
     * @brief Destroy printer state manager
     *
     * Cleans up LVGL subjects and releases resources.
     */
    ~PrinterState();

    /**
     * @brief Initialize all LVGL subjects
     *
     * MUST be called BEFORE creating XML components that bind to these subjects.
     * Can be called multiple times safely - subsequent calls are ignored.
     *
     * @param register_xml If true, registers subjects with LVGL XML system (default).
     *                     Set to false in tests to avoid XML observer creation.
     */
    void init_subjects(bool register_xml = true);

    /**
     * @brief Deinitialize all subjects across all state components
     *
     * Cascades to all 13 sub-component deinit_subjects() methods and
     * then deinitializes PrinterState's own subjects.
     */
    void deinit_subjects();

    /**
     * @brief Re-register temperature subjects with LVGL XML system
     *
     * FOR TESTING ONLY. Call this to ensure temperature subjects are registered
     * in LVGL's global XML registry. Use when other tests may have overwritten
     * the registry with their own PrinterState instances.
     *
     * Does NOT reinitialize subjects - only updates LVGL XML registry mappings.
     */
    void register_temperature_xml_subjects();

    /**
     * @brief Update state from Moonraker notification
     *
     * Extracts values from notify_status_update messages and updates subjects.
     * Also maintains JSON cache for complex data.
     *
     * @param notification Parsed JSON notification from Moonraker
     */
    void update_from_notification(const json& notification);

    /**
     * @brief Update state from raw status data
     *
     * Updates subjects from a printer status object. Can be called directly
     * with subscription response data or extracted from notifications.
     * This is the core update logic used by both initial state and notifications.
     *
     * @param status Printer status object (e.g., from result.status or params[0])
     */
    void update_from_status(const json& status);

    //
    // Subject accessors for XML binding
    //

    // Temperature subjects (centidegrees: value * 10 for 0.1C resolution)
    // Example: 205.3C is stored as 2053. Divide by 10 for display.
    // Delegated to PrinterTemperatureState component.

    // Active extruder subjects — track whichever extruder is currently active
    lv_subject_t* get_active_extruder_temp_subject() {
        return temperature_state_.get_active_extruder_temp_subject();
    }
    lv_subject_t* get_active_extruder_target_subject() {
        return temperature_state_.get_active_extruder_target_subject();
    }

    // Multi-extruder discovery
    void init_extruders(const std::vector<std::string>& heaters) {
        temperature_state_.init_extruders(heaters);
    }

    // Per-extruder subject access (returns nullptr if not found)
    // Prefer the overloads with SubjectLifetime when creating observers!
    lv_subject_t* get_extruder_temp_subject(const std::string& name) {
        return temperature_state_.get_extruder_temp_subject(name);
    }
    lv_subject_t* get_extruder_target_subject(const std::string& name) {
        return temperature_state_.get_extruder_target_subject(name);
    }
    lv_subject_t* get_extruder_temp_subject(const std::string& name, SubjectLifetime& lifetime) {
        return temperature_state_.get_extruder_temp_subject(name, lifetime);
    }
    lv_subject_t* get_extruder_target_subject(const std::string& name, SubjectLifetime& lifetime) {
        return temperature_state_.get_extruder_target_subject(name, lifetime);
    }

    int extruder_count() const {
        return temperature_state_.extruder_count();
    }

    const std::string& active_extruder_name() const {
        return temperature_state_.active_extruder_name();
    }

    void set_active_extruder(const std::string& name) {
        temperature_state_.set_active_extruder(name);
    }

    lv_subject_t* get_extruder_version_subject() {
        return temperature_state_.get_extruder_version_subject();
    }

    // Direct access to temperature state (for UI enumeration)
    const helix::PrinterTemperatureState& temperature_state() const {
        return temperature_state_;
    }

    lv_subject_t* get_bed_temp_subject() {
        return temperature_state_.get_bed_temp_subject();
    }
    lv_subject_t* get_bed_target_subject() {
        return temperature_state_.get_bed_target_subject();
    }
    lv_subject_t* get_chamber_temp_subject() {
        return temperature_state_.get_chamber_temp_subject();
    }
    lv_subject_t* get_chamber_target_subject() {
        return temperature_state_.get_chamber_target_subject();
    }

    // Print progress subjects - delegated to PrinterPrintState component
    lv_subject_t* get_print_progress_subject() {
        return print_domain_.get_print_progress_subject();
    } // 0-100
    lv_subject_t* get_print_filename_subject() {
        return print_domain_.get_print_filename_subject();
    }
    lv_subject_t* get_print_state_subject() {
        return print_domain_.get_print_state_subject();
    } // "standby", "printing", "paused", "complete" (string for UI display)

    /**
     * @brief Get print thumbnail path subject for UI binding
     *
     * String subject holding the LVGL path to the current print's thumbnail.
     * Set by PrintStatusPanel when thumbnail loads, cleared when print ends.
     * HomePanel observes this to show the same thumbnail on the print card.
     *
     * @return Pointer to string subject
     */
    lv_subject_t* get_print_thumbnail_path_subject() {
        return print_domain_.get_print_thumbnail_path_subject();
    }

    /**
     * @brief Set the current print's thumbnail path
     *
     * Called by PrintStatusPanel after successfully loading a thumbnail.
     * This allows other UI components (e.g., HomePanel) to display the
     * same thumbnail without duplicating the loading logic.
     *
     * @param path LVGL-compatible path (e.g., "A:/tmp/thumbnail_xxx.bin")
     */
    void set_print_thumbnail_path(const std::string& path);

    /**
     * @brief Get print job state enum subject
     *
     * Integer subject holding PrintJobState enum value for type-safe comparisons.
     * Use this for logic, use get_print_state_subject() for UI display binding.
     *
     * @return Pointer to integer subject (cast value to PrintJobState)
     */
    lv_subject_t* get_print_state_enum_subject() {
        return print_domain_.get_print_state_enum_subject();
    }

    /**
     * @brief Get print active subject for UI binding
     *
     * Integer subject: 1 when PRINTING or PAUSED, 0 otherwise.
     * Derived from print_state_enum for simpler XML bindings (avoids OR logic).
     * Use for card visibility that should show during any active print.
     *
     * @return Pointer to integer subject (0 or 1)
     */
    lv_subject_t* get_print_active_subject() {
        return print_domain_.get_print_active_subject();
    }

    /**
     * @brief Get print outcome subject for UI binding
     *
     * Integer subject holding PrintOutcome enum value for terminal print state.
     * Unlike print_state_enum (which reflects live Moonraker state), print_outcome
     * persists how the last print ended until a new print starts.
     *
     * Use this for showing completion/cancellation UI (badges, reprint buttons)
     * that should persist after Moonraker transitions back to STANDBY.
     *
     * @return Pointer to integer subject (cast value to PrintOutcome)
     */
    lv_subject_t* get_print_outcome_subject() {
        return print_domain_.get_print_outcome_subject();
    }

    /**
     * @brief Set print outcome for UI badge display
     *
     * Call this to manually set the print outcome (e.g., from AbortManager
     * when Moonraker reports "standby" instead of "cancelled" after M112).
     *
     * @param outcome The print outcome value to set
     */
    void set_print_outcome(PrintOutcome outcome);

    /**
     * @brief Get subject for showing print progress card on home panel
     *
     * Combined subject: 1 when print_active==1 AND print_start_phase==0.
     * Simplifies XML bindings by avoiding conflicting multi-binding logic.
     *
     * @return Pointer to integer subject (0 or 1)
     */
    lv_subject_t* get_print_show_progress_subject() {
        return print_domain_.get_print_show_progress_subject();
    }

    /**
     * @brief Get subject for display-ready print filename
     *
     * Clean filename without path or .helix_temp prefix, suitable for UI display.
     * Set by PrintStatusPanel when processing raw print_filename.
     *
     * @return Pointer to string subject
     */
    lv_subject_t* get_print_display_filename_subject() {
        return print_domain_.get_print_display_filename_subject();
    }

    /**
     * @brief Set display-ready print filename for UI binding
     *
     * Called by PrintStatusPanel after cleaning up the raw filename.
     *
     * @param name Clean display name (e.g., "Body1" not ".helix_temp/modified_123_Body1.gcode")
     */
    void set_print_display_filename(const std::string& name);

    /**
     * @brief Get current print job state as enum
     *
     * Convenience method for direct enum access without subject lookup.
     *
     * @return Current PrintJobState
     */
    PrintJobState get_print_job_state() const;

    /**
     * @brief Check if a new print can be started
     *
     * Returns true if the printer is in a state that allows starting a new print.
     * A print can be started when the printer is idle (STANDBY), a previous print
     * finished (COMPLETE, CANCELLED), or the printer recovered from an error (ERROR).
     * Also checks that no print workflow is currently in progress (e.g., G-code
     * downloading/modifying/uploading).
     *
     * @return true if start_print() can be called safely
     */
    [[nodiscard]] bool can_start_new_print() const;

    /**
     * @brief Set the print-in-progress flag (UI workflow state)
     *
     * Call with true when starting the print preparation workflow
     * (downloading/modifying/uploading G-code), and false when complete.
     * This flag is checked by can_start_new_print() to prevent:
     * - Double-tap issues during long G-code modification workflows
     * - UI elements from indicating "ready to print" during preparation
     * - Race conditions from concurrent print requests
     *
     * Updates the print_in_progress_ subject so UI observers can react.
     *
     * Thread-safe: Uses helix::ui::queue_update() to defer LVGL subject updates
     * to the main thread. Can be safely called from WebSocket callbacks.
     */
    void set_print_in_progress(bool in_progress);

    /**
     * @brief Check if a print workflow is currently in progress
     *
     * Returns true during print preparation (G-code download/modify/upload),
     * even though the printer's physical state may still be STANDBY.
     */
    [[nodiscard]] bool is_print_in_progress() const {
        return print_domain_.is_print_in_progress();
    }

    /**
     * @brief Reset UI state when starting a new print
     *
     * Clears the print_complete flag and resets progress to prepare for
     * a new print. Call this BEFORE navigating to print status panel.
     */
    void reset_for_new_print();

    /**
     * @brief Get the print-in-progress subject for observing workflow state
     *
     * Value is 1 when print preparation is in progress, 0 otherwise.
     */
    lv_subject_t* get_print_in_progress_subject() {
        return print_domain_.get_print_in_progress_subject();
    }

    // Filament used subject (from print_stats.filament_used, in mm)
    // Delegated to PrinterPrintState component
    lv_subject_t* get_print_filament_used_subject() {
        return print_domain_.get_print_filament_used_subject();
    }

    // Layer tracking subjects (from print_stats.info.current_layer/total_layer)
    // Delegated to PrinterPrintState component
    lv_subject_t* get_print_layer_current_subject() {
        return print_domain_.get_print_layer_current_subject();
    }
    lv_subject_t* get_print_layer_total_subject() {
        return print_domain_.get_print_layer_total_subject();
    }

    /**
     * @brief Set total layer count from file metadata
     *
     * Called when print starts to initialize total layers from file metadata.
     * Moonraker notifications may update this later via SET_PRINT_STATS_INFO.
     */
    void set_print_layer_total(int total) {
        print_domain_.set_print_layer_total(total);
    }

    /**
     * @brief Set current layer number (gcode response fallback)
     *
     * Thread-safe. Called from gcode response parser when
     * print_stats.info.current_layer doesn't fire.
     */
    void set_print_layer_current(int layer) {
        print_domain_.set_print_layer_current(layer);
    }

    /**
     * @brief Check if real layer data has been received from slicer/Moonraker.
     * When false, layer count is estimated from print progress.
     */
    bool has_real_layer_data() const {
        return print_domain_.has_real_layer_data();
    }

    /**
     * @brief Set slicer's estimated total print time (from file metadata)
     *
     * Used as fallback for remaining time when print_duration is still 0.
     */
    void set_estimated_print_time(int seconds) {
        print_domain_.set_estimated_print_time(seconds);
    }

    /**
     * @brief Get slicer's estimated total print time
     */
    int get_estimated_print_time() const {
        return print_domain_.get_estimated_print_time();
    }

    // Print time tracking subjects (in seconds) - delegated to PrinterPrintState
    lv_subject_t* get_print_duration_subject() {
        return print_domain_.get_print_duration_subject();
    }
    lv_subject_t* get_print_elapsed_subject() {
        return print_domain_.get_print_elapsed_subject();
    }
    lv_subject_t* get_print_time_left_subject() {
        return print_domain_.get_print_time_left_subject();
    }

    // ========================================================================
    // PRINT START PROGRESS (detected from G-code response during PRINT_START)
    // ========================================================================

    /**
     * @brief Get print start phase subject for UI binding
     *
     * Integer subject holding PrintStartPhase enum value.
     * Use with bind_flag_if_eq/not_eq in XML to show/hide progress overlay.
     */
    lv_subject_t* get_print_start_phase_subject() {
        return print_domain_.get_print_start_phase_subject();
    }

    /**
     * @brief Get print start message subject for UI binding
     *
     * String subject with human-readable phase description (e.g., "Heating Nozzle...").
     * Use with bind_text in XML.
     */
    lv_subject_t* get_print_start_message_subject() {
        return print_domain_.get_print_start_message_subject();
    }

    /**
     * @brief Get print start progress subject for UI binding
     *
     * Integer subject with 0-100% progress based on weighted phase completion.
     * Use with bind_value on lv_bar in XML.
     */
    lv_subject_t* get_print_start_progress_subject() {
        return print_domain_.get_print_start_progress_subject();
    }

    /**
     * @brief Get predicted pre-print time remaining subject for UI binding
     *
     * String subject with formatted remaining time (e.g., "~2 min left").
     * Empty when no prediction is available.
     */
    lv_subject_t* get_print_start_time_left_subject() {
        return print_domain_.get_print_start_time_left_subject();
    }

    /**
     * @brief Set predicted pre-print time remaining (main-thread only)
     */
    void set_print_start_time_left(const char* text) {
        print_domain_.set_print_start_time_left(text);
    }

    /**
     * @brief Clear predicted pre-print time remaining
     */
    void clear_print_start_time_left() {
        print_domain_.clear_print_start_time_left();
    }

    /**
     * @brief Get pre-print remaining seconds subject for augmenting total remaining
     */
    lv_subject_t* get_preprint_remaining_subject() {
        return print_domain_.get_preprint_remaining_subject();
    }

    /**
     * @brief Set pre-print remaining seconds (main-thread only)
     */
    void set_preprint_remaining_seconds(int seconds) {
        print_domain_.set_preprint_remaining_seconds(seconds);
    }

    /**
     * @brief Get pre-print elapsed seconds subject
     */
    lv_subject_t* get_preprint_elapsed_subject() {
        return print_domain_.get_preprint_elapsed_subject();
    }

    /**
     * @brief Set pre-print elapsed seconds (main-thread only)
     */
    void set_preprint_elapsed_seconds(int seconds) {
        print_domain_.set_preprint_elapsed_seconds(seconds);
    }

    /// Klipper display message from M117 / display_status.message
    lv_subject_t* get_display_message_subject() {
        return print_domain_.get_display_message_subject();
    }

    /// 1 when display_message is non-empty, 0 when empty
    lv_subject_t* get_display_message_visible_subject() {
        return print_domain_.get_display_message_visible_subject();
    }

    /**
     * @brief Check if currently in print start phase
     *
     * Convenience method to check if we're showing PRINT_START progress.
     *
     * @return true if phase is not IDLE
     */
    bool is_in_print_start() const;

    /**
     * @brief Set print start phase and update message/progress
     *
     * Called by PrintStartCollector when phases are detected.
     * Updates all three subjects: phase, message, and progress.
     *
     * @param phase Current PrintStartPhase
     * @param message Human-readable message (e.g., "Heating Nozzle...")
     * @param progress Estimated progress 0-100%
     */
    void set_print_start_state(PrintStartPhase phase, const char* message, int progress);

    /**
     * @brief Reset print start to IDLE
     *
     * Called when print initialization completes or print is cancelled.
     */
    void reset_print_start_state();

    // Toolhead position subjects - actual physical position (includes mesh compensation)
    lv_subject_t* get_position_x_subject() {
        return motion_state_.get_position_x_subject();
    }
    lv_subject_t* get_position_y_subject() {
        return motion_state_.get_position_y_subject();
    }
    lv_subject_t* get_position_z_subject() {
        return motion_state_.get_position_z_subject();
    }

    // Gcode position subjects - commanded position (what user requested)
    lv_subject_t* get_gcode_position_x_subject() {
        return motion_state_.get_gcode_position_x_subject();
    }
    lv_subject_t* get_gcode_position_y_subject() {
        return motion_state_.get_gcode_position_y_subject();
    }
    lv_subject_t* get_gcode_position_z_subject() {
        return motion_state_.get_gcode_position_z_subject();
    }

    lv_subject_t* get_homed_axes_subject() {
        return motion_state_.get_homed_axes_subject();
    } // "xyz", "xy", etc.
    // Note: Derived subjects (xy_homed, z_homed, all_homed) are panel-local in ControlsPanel

    // Speed/Flow subjects (percentages, 0-100) - delegated to PrinterMotionState component
    lv_subject_t* get_speed_factor_subject() {
        return motion_state_.get_speed_factor_subject();
    }
    lv_subject_t* get_flow_factor_subject() {
        return motion_state_.get_flow_factor_subject();
    }
    lv_subject_t* get_fan_speed_subject() {
        return fan_state_.get_fan_speed_subject();
    }

    // ========================================================================
    // MULTI-FAN API - Delegated to PrinterFanState component
    // ========================================================================

    /**
     * @brief Get all tracked fans
     * @return Const reference to fan info vector
     */
    const std::vector<helix::FanInfo>& get_fans() const {
        return fan_state_.get_fans();
    }

    /**
     * @brief Get fans version subject for UI change notification
     *
     * Incremented when fan list changes or speeds update.
     * UI should observe this to rebuild dynamic fan list.
     */
    lv_subject_t* get_fans_version_subject() {
        return fan_state_.get_fans_version_subject();
    }

    /**
     * @brief Get speed subject for a specific fan (with lifetime token for observer safety)
     *
     * IMPORTANT: Use this overload when creating observers on the returned subject.
     * Dynamic fan subjects may be destroyed during reconnection — the lifetime token
     * prevents use-after-free crashes in ObserverGuard.
     *
     * @param object_name Moonraker object name (e.g., "fan", "heater_fan hotend_fan")
     * @param[out] lifetime Receives the subject's lifetime token
     * @return Pointer to subject, or nullptr if fan not found
     */
    lv_subject_t* get_fan_speed_subject(const std::string& object_name, SubjectLifetime& lifetime) {
        return fan_state_.get_fan_speed_subject(object_name, lifetime);
    }

    /// Get speed subject without lifetime token (only for non-observer uses like reading values)
    lv_subject_t* get_fan_speed_subject(const std::string& object_name) {
        return fan_state_.get_fan_speed_subject(object_name);
    }

    /**
     * @brief Initialize fan list from discovered fan objects
     * @param fan_objects List of Moonraker fan object names
     * @param roles Wizard-configured fan role assignments
     */
    void init_fans(const std::vector<std::string>& fan_objects,
                   const helix::FanRoleConfig& roles = {}) {
        fan_state_.init_fans(fan_objects, roles);
    }

    /**
     * @brief Update speed for a specific fan (optimistic UI updates)
     * @param object_name Moonraker object name (e.g., "fan_generic chamber_fan")
     * @param speed Speed as 0.0-1.0 (Moonraker format)
     */
    void update_fan_speed(const std::string& object_name, double speed) {
        fan_state_.update_fan_speed(object_name, speed);
    }

    /**
     * @brief Get G-code Z offset subject for tune panel
     *
     * Returns current Z-offset from gcode_move.homing_origin[2] in microns.
     * Divide by 1000.0 to get mm value (e.g., 200 = 0.200mm).
     * Used for live baby-stepping display during prints.
     * Delegated to PrinterMotionState component.
     */
    lv_subject_t* get_gcode_z_offset_subject() {
        return motion_state_.get_gcode_z_offset_subject();
    }

    // ========================================================================
    // PENDING Z-OFFSET DELTA (for tracking adjustments made during print)
    // Delegated to PrinterMotionState component.
    // ========================================================================

    /**
     * @brief Get pending Z-offset delta subject
     *
     * Returns accumulated Z-offset adjustment made during print tuning (microns).
     * Use this to show "unsaved adjustment" notification in Controls panel.
     */
    lv_subject_t* get_pending_z_offset_delta_subject() {
        return motion_state_.get_pending_z_offset_delta_subject();
    }

    /**
     * @brief Add to pending Z-offset delta (called when user adjusts Z during print)
     * @param delta_microns Adjustment in microns (positive = farther, negative = closer)
     */
    void add_pending_z_offset_delta(int delta_microns) {
        motion_state_.add_pending_z_offset_delta(delta_microns);
    }

    /**
     * @brief Get current pending Z-offset delta in microns
     */
    int get_pending_z_offset_delta() const {
        return motion_state_.get_pending_z_offset_delta();
    }

    /**
     * @brief Check if there's a pending Z-offset adjustment
     */
    bool has_pending_z_offset_adjustment() const {
        return motion_state_.has_pending_z_offset_adjustment();
    }

    /**
     * @brief Clear pending Z-offset delta (after save or dismiss)
     */
    void clear_pending_z_offset_delta() {
        motion_state_.clear_pending_z_offset_delta();
    }

    // Printer connection state subjects (Moonraker WebSocket) - delegated to PrinterNetworkState
    lv_subject_t* get_printer_connection_state_subject() {
        return network_state_.get_printer_connection_state_subject();
    } // 0=disconnected, 1=connecting, 2=connected, 3=reconnecting, 4=failed
    lv_subject_t* get_printer_connection_message_subject() {
        return network_state_.get_printer_connection_message_subject();
    } // Status message

    // Network connectivity subject (WiFi/Ethernet) - delegated to PrinterNetworkState
    lv_subject_t* get_network_status_subject() {
        return network_state_.get_network_status_subject();
    } // 0=disconnected, 1=connecting, 2=connected (matches NetworkStatus enum)

    // Klipper firmware state subject - delegated to PrinterNetworkState
    lv_subject_t* get_klippy_state_subject() {
        return network_state_.get_klippy_state_subject();
    } // 0=ready, 1=startup, 2=shutdown, 3=error (matches KlippyState enum)

    // Combined nav button enabled subject (for navbar icon visibility) - delegated to
    // PrinterNetworkState
    lv_subject_t* get_nav_buttons_enabled_subject() {
        return network_state_.get_nav_buttons_enabled_subject();
    } // 1=enabled (connected AND klippy ready), 0=disabled

    // LED state subjects - delegated to PrinterLedState component
    lv_subject_t* get_led_state_subject() {
        return led_state_component_.get_led_state_subject();
    } // 0=off, 1=on (derived from LED color data)

    // LED RGBW channel subjects (0-255 integer range)
    lv_subject_t* get_led_r_subject() {
        return led_state_component_.get_led_r_subject();
    }
    lv_subject_t* get_led_g_subject() {
        return led_state_component_.get_led_g_subject();
    }
    lv_subject_t* get_led_b_subject() {
        return led_state_component_.get_led_b_subject();
    }
    lv_subject_t* get_led_w_subject() {
        return led_state_component_.get_led_w_subject();
    }
    lv_subject_t* get_led_brightness_subject() {
        return led_state_component_.get_led_brightness_subject();
    } // 0-100 (max of RGBW channels)

    /**
     * @brief Get excluded objects version subject
     *
     * This subject is incremented whenever the excluded objects list changes.
     * Observers should watch this subject and call get_excluded_objects() to
     * get the updated list when notified.
     *
     * @return Subject pointer (integer, incremented on each change)
     */
    lv_subject_t* get_excluded_objects_version_subject() {
        return excluded_objects_state_.get_excluded_objects_version_subject();
    }

    /**
     * @brief Get the current set of excluded objects
     *
     * Returns object names that have been excluded from printing via Klipper's
     * EXCLUDE_OBJECT feature. Updated from Moonraker notify_status_update.
     *
     * @return Reference to the set of excluded object names
     */
    const std::unordered_set<std::string>& get_excluded_objects() const {
        return excluded_objects_state_.get_excluded_objects();
    }

    /**
     * @brief Get the list of all defined objects in the current print
     *
     * Returns all object names from Klipper's exclude_object status.
     *
     * @return Const reference to the vector of defined object names
     */
    const std::vector<std::string>& get_defined_objects() const {
        return excluded_objects_state_.get_defined_objects();
    }

    /**
     * @brief Get the name of the currently printing object
     *
     * @return Const reference to current object name, or empty string if none
     */
    const std::string& get_current_object() const {
        return excluded_objects_state_.get_current_object();
    }

    /**
     * @brief Get defined objects version subject
     *
     * Incremented whenever the defined objects list changes.
     *
     * @return Subject pointer (integer, incremented on each change)
     */
    lv_subject_t* get_defined_objects_version_subject() {
        return excluded_objects_state_.get_defined_objects_version_subject();
    }

    /**
     * @brief Check if any objects are defined for exclude_object
     *
     * @return true if the print has defined objects available for exclusion
     */
    bool has_exclude_objects() const {
        return excluded_objects_state_.has_objects();
    }

    /**
     * @brief Update excluded objects from Moonraker status update
     *
     * Called by status update handler when exclude_object.excluded_objects changes.
     * Increments the version subject to notify observers.
     *
     * @param objects Set of object names that are currently excluded
     */
    void set_excluded_objects(const std::unordered_set<std::string>& objects);

    /**
     * @brief Set which LED to track for state updates
     *
     * Call this after loading config to tell PrinterState which LED object
     * to monitor from Moonraker notifications. The LED name should match
     * the Klipper config (e.g., "neopixel chamber_light", "led status_led").
     *
     * @param led_name Full LED name including type prefix, or empty to disable
     */
    void set_tracked_led(const std::string& led_name) {
        led_state_component_.set_tracked_led(led_name);
    }

    /**
     * @brief Get the currently tracked LED name
     *
     * @return LED name being tracked, or empty string if none
     */
    std::string get_tracked_led() const {
        return led_state_component_.get_tracked_led();
    }

    /**
     * @brief Check if an LED is configured for tracking
     *
     * @return true if a LED name has been set
     */
    bool has_tracked_led() const {
        return led_state_component_.has_tracked_led();
    }

    /**
     * @brief Set printer connection state (Moonraker WebSocket)
     *
     * Updates both printer_connection_state and printer_connection_message subjects.
     * Called by main.cpp WebSocket callbacks.
     *
     * @param state 0=disconnected, 1=connecting, 2=connected, 3=reconnecting, 4=failed
     * @param message Status message ("Connecting...", "Ready", "Disconnected", etc.)
     */
    void set_printer_connection_state(int state, const char* message);

    /**
     * @brief Internal: set connection state on main thread
     * @note Called via ui_async_call from set_printer_connection_state()
     */
    void set_printer_connection_state_internal(int state, const char* message);

    /**
     * @brief Check if printer has ever connected this session
     *
     * Returns true if we've successfully connected to Moonraker at least once.
     * Used to distinguish "never connected" (gray icon) from "disconnected after
     * being connected" (yellow warning icon).
     */
    bool was_ever_connected() const {
        return network_state_.was_ever_connected();
    }

    /**
     * @brief Set Klipper firmware state (thread-safe, async)
     *
     * Updates klippy_state subject via lv_async_call to ensure thread safety.
     * Called when Moonraker sends klippy state notifications from WebSocket
     * callbacks (notify_klippy_ready, notify_klippy_disconnected).
     *
     * @param state KlippyState enum value
     */
    void set_klippy_state(KlippyState state);

    /**
     * @brief Set Klipper firmware state (synchronous, main-thread only)
     *
     * Directly updates klippy_state subject without async deferral.
     * Only call this from the main LVGL thread. Use for testing or when
     * already on the main thread.
     *
     * @param state KlippyState enum value
     */
    void set_klippy_state_sync(KlippyState state);

    /**
     * @brief Set network connectivity status
     *
     * Updates network_status_ subject based on WiFi/Ethernet availability.
     * Called periodically from main.cpp to reflect actual network state.
     *
     * @param status 0=DISCONNECTED, 1=CONNECTING, 2=CONNECTED (NetworkStatus enum)
     */
    void set_network_status(int status);

    /**
     * @brief Update printer capability subjects from PrinterDiscovery
     *
     * Updates subjects that control visibility of pre-print option checkboxes.
     * Applies user-configured overrides from helixconfig.json before updating subjects.
     * Called by main.cpp after MoonrakerClient::discover_printer() completes.
     *
     * @param hardware PrinterDiscovery populated from printer.objects.list
     */
    void set_hardware(const helix::PrinterDiscovery& hardware);

    /**
     * @brief Set Klipper software version from printer.info
     *
     * Updates klipper_version_ subject for Settings panel About section.
     * Called by main.cpp after MoonrakerClient::discover_printer() completes.
     *
     * @param version Version string (e.g., "v0.12.0-108-g2c7a9d58")
     */
    void set_klipper_version(const std::string& version);

    /**
     * @brief Set Moonraker software version from server.info
     *
     * Updates moonraker_version_ subject for Settings panel About section.
     * Called by main.cpp after MoonrakerClient::discover_printer() completes.
     *
     * @param version Version string (e.g., "v0.8.0-143-g2c7a9d58")
     */
    void set_moonraker_version(const std::string& version);

    /**
     * @brief Set OS version from machine.system_info
     *
     * Updates os_version_ subject for Settings panel About section.
     * Called after MoonrakerClient::discover_printer() completes.
     *
     * @param version OS distribution name (e.g., "Forge-X 1.4.0")
     */
    void set_os_version(const std::string& version);

    /**
     * @brief Get Klipper version subject for XML binding
     */
    lv_subject_t* get_klipper_version_subject() {
        return versions_state_.get_klipper_version_subject();
    }

    /**
     * @brief Get Moonraker version subject for XML binding
     */
    lv_subject_t* get_moonraker_version_subject() {
        return versions_state_.get_moonraker_version_subject();
    }

    /**
     * @brief Get OS version subject for XML binding
     */
    lv_subject_t* get_os_version_subject() {
        return versions_state_.get_os_version_subject();
    }

    /**
     * @brief Get the capability overrides for external access
     *
     * Allows other components to check effective capability availability
     * with user overrides applied.
     *
     * @return Reference to the CapabilityOverrides instance
     */
    [[nodiscard]] const CapabilityOverrides& get_capability_overrides() const {
        return capability_overrides_;
    }

    /**
     * @brief Set power device count
     *
     * Delegates to PrinterCapabilitiesState (thread-safe).
     *
     * @param count Number of discovered power devices
     */
    void set_power_device_count(int count);

    /**
     * @brief Get power device count subject for XML binding
     *
     * Integer subject holding the number of discovered power devices.
     * 0 = no power devices, used to hide/show power panel UI elements.
     */
    lv_subject_t* get_power_device_count_subject() {
        return capabilities_state_.get_power_device_count_subject();
    }

    /**
     * @brief Set Spoolman availability status
     *
     * Called after checking Moonraker's server.info components and verifying
     * Spoolman connection via get_spoolman_status(). Updates printer_has_spoolman_
     * subject for UI visibility gating.
     *
     * Thread-safe: Can be called from any thread, defers LVGL update to main thread.
     *
     * @param available True if Spoolman is configured and connected
     */
    void set_spoolman_available(bool available);

    /**
     * @brief Set webcam availability status
     *
     * Called after checking Moonraker's server.webcams.list API.
     * Updates printer_has_webcam subject for UI visibility gating.
     *
     * Thread-safe: Can be called from any thread, defers LVGL update to main thread.
     *
     * @param available True if at least one enabled webcam is configured
     * @param stream_url MJPEG stream URL of first enabled webcam
     * @param snapshot_url Snapshot URL of first enabled webcam
     */
    void set_webcam_available(bool available, const std::string& stream_url = "",
                              const std::string& snapshot_url = "",
                              bool flip_h = false, bool flip_v = false);

    /// Get MJPEG stream URL of first enabled webcam
    const std::string& get_webcam_stream_url() const {
        return capabilities_state_.get_webcam_stream_url();
    }

    /// Get snapshot URL of first enabled webcam
    const std::string& get_webcam_snapshot_url() const {
        return capabilities_state_.get_webcam_snapshot_url();
    }

    /// Webcam flip flags from Moonraker config
    bool get_webcam_flip_horizontal() const {
        return capabilities_state_.get_webcam_flip_horizontal();
    }
    bool get_webcam_flip_vertical() const {
        return capabilities_state_.get_webcam_flip_vertical();
    }

    /**
     * @brief Set timelapse plugin availability status
     *
     * Called after verifying the moonraker-timelapse plugin is installed.
     * Updates printer_has_timelapse subject for UI visibility gating.
     *
     * Thread-safe: Can be called from any thread, defers LVGL update to main thread.
     *
     * @param available True if moonraker-timelapse plugin is installed and responding
     */
    void set_timelapse_available(bool available);

    /**
     * @brief Set HelixPrint plugin installation status
     *
     * Called after checking Moonraker for the helix_print plugin.
     * Updates helix_plugin_installed_ subject for UI visibility gating.
     *
     * Thread-safe: Can be called from any thread, defers LVGL update to main thread.
     *
     * @param installed True if HelixPrint plugin is installed
     */
    void set_helix_plugin_installed(bool installed);

    /**
     * @brief Check if HelixPrint plugin is available
     *
     * Convenience getter for checking plugin status. This is the preferred
     * way to query plugin availability (vs accessing the subject directly).
     *
     * @return True if the HelixPrint Moonraker plugin is installed
     */
    bool service_has_helix_plugin() const;

    /**
     * @brief Set phase tracking enabled/disabled status
     *
     * Called after querying the plugin's phase tracking status.
     * Updates phase_tracking_enabled_ subject for UI toggle state.
     *
     * Thread-safe: Can be called from any thread, defers LVGL update to main thread.
     *
     * @param enabled True if phase tracking is enabled
     */
    void set_phase_tracking_enabled(bool enabled);

    /**
     * @brief Check if phase tracking is enabled
     *
     * @return True if phase tracking is enabled, false otherwise
     */
    bool is_phase_tracking_enabled() const;

    /**
     * @brief Get helix_plugin_installed subject for observers
     *
     * Use this when you need to observe plugin status changes (e.g., for install prompts).
     *
     * @return Pointer to the helix_plugin_installed_ subject
     */
    lv_subject_t* get_helix_plugin_installed_subject() {
        return plugin_status_state_.get_helix_plugin_installed_subject();
    }

    /**
     * @brief Get phase_tracking_enabled subject for observers
     *
     * Use this when you need to observe phase tracking status changes.
     *
     * @return Pointer to the phase_tracking_enabled_ subject
     */
    lv_subject_t* get_phase_tracking_enabled_subject() {
        return plugin_status_state_.get_phase_tracking_enabled_subject();
    }

    // === Visibility Subject Getters (LT2: for pre-print option row visibility) ===

    /**
     * @brief Get visibility subject for bed mesh row
     *
     * Returns 1 when bed mesh option should be visible (helix_plugin_installed AND
     * printer_has_bed_mesh), 0 otherwise.
     */
    lv_subject_t* get_can_show_bed_mesh_subject() {
        return composite_visibility_state_.get_can_show_bed_mesh_subject();
    }

    /**
     * @brief Get visibility subject for QGL row
     */
    lv_subject_t* get_can_show_qgl_subject() {
        return composite_visibility_state_.get_can_show_qgl_subject();
    }

    /**
     * @brief Get visibility subject for Z-tilt row
     */
    lv_subject_t* get_can_show_z_tilt_subject() {
        return composite_visibility_state_.get_can_show_z_tilt_subject();
    }

    /**
     * @brief Get visibility subject for nozzle clean row
     */
    lv_subject_t* get_can_show_nozzle_clean_subject() {
        return composite_visibility_state_.get_can_show_nozzle_clean_subject();
    }

    /**
     * @brief Get aggregate subject: 1 if any preprint option row is visible
     */
    lv_subject_t* get_has_any_preprint_options_subject() {
        return composite_visibility_state_.get_has_any_preprint_options_subject();
    }

    /**
     * @brief Get visibility subject for timelapse row
     *
     * Returns 1 when printer has timelapse plugin installed, 0 otherwise.
     * Note: Unlike other can_show_* subjects, timelapse doesn't require helix_print plugin.
     */
    lv_subject_t* get_printer_has_timelapse_subject() {
        return capabilities_state_.get_printer_has_timelapse_subject();
    }

    /**
     * @brief Get capability subject for purge line (priming)
     *
     * Returns 1 when printer has purge/priming capability, 0 otherwise.
     */
    lv_subject_t* get_printer_has_purge_line_subject() {
        return capabilities_state_.get_printer_has_purge_line_subject();
    }

    /**
     * @brief Get visibility subject for purge line row
     *
     * Returns 1 when purge line option should be visible (helix_plugin_installed AND
     * printer_has_purge_line), 0 otherwise.
     */
    lv_subject_t* get_can_show_purge_line_subject() {
        return composite_visibility_state_.get_can_show_purge_line_subject();
    }

    /**
     * @brief Set printer kinematics type and update bed_moves subject
     *
     * Updates printer_bed_moves_ subject based on kinematics type.
     * CoreXY printers typically have bed moving on Z (Voron 2.4, RatRig).
     * Cartesian/Delta printers typically have gantry moving on Z (Ender 3, Prusa).
     *
     * @param kinematics Kinematics type string from toolhead config
     */
    void set_kinematics(const std::string& kinematics);

    /**
     * @brief Apply effective bed_moves value based on Z movement style override
     *
     * Reads ZMovementStyle from SettingsManager and applies:
     * - AUTO: uses auto_detected_bed_moves_ from kinematics detection
     * - BED_MOVES: forces printer_bed_moves = true
     * - NOZZLE_MOVES: forces printer_bed_moves = false
     *
     * Called from set_kinematics() and SettingsManager::set_z_movement_style().
     */
    void apply_effective_bed_moves();

    /**
     * @brief Get bed_moves subject for XML binding
     *
     * Returns 1 if the printer's bed moves on Z axis (corexy, corexz),
     * 0 if the printer's gantry/head moves on Z (cartesian, delta).
     * Used for Z-offset UI to show appropriate directional icons.
     */
    lv_subject_t* get_printer_bed_moves_subject() {
        return capabilities_state_.get_printer_bed_moves_subject();
    }

    /**
     * @brief Get printer_has_chamber_heater subject
     *
     * Returns 1 if the printer has an active chamber heater (heater_generic chamber),
     * 0 if chamber is sensor-only or absent. Used by chamber temp overlay to
     * show/hide preset controls.
     */
    lv_subject_t* get_printer_has_chamber_heater_subject() {
        return capabilities_state_.get_printer_has_chamber_heater_subject();
    }

    /**
     * @brief Get manual probe active subject for Z-offset calibration
     *
     * Returns 1 when Klipper is in manual probe mode (PROBE_CALIBRATE,
     * Z_ENDSTOP_CALIBRATE), 0 otherwise. Used by ZOffsetCalibrationPanel
     * to transition from PROBING to ADJUSTING state.
     */
    lv_subject_t* get_manual_probe_active_subject() {
        return calibration_state_.get_manual_probe_active_subject();
    }

    /**
     * @brief Get manual probe Z position subject
     *
     * Returns current Z position during manual probe (in microns, multiply
     * by 0.001 to get mm). Updated in real-time by Klipper as TESTZ
     * commands are executed.
     */
    lv_subject_t* get_manual_probe_z_position_subject() {
        return calibration_state_.get_manual_probe_z_position_subject();
    }

    /**
     * @brief Get motors enabled subject for UI binding
     *
     * Returns 1 when stepper motors are enabled (idle_timeout.state is "Ready" or "Printing"),
     * 0 when motors are disabled (idle_timeout.state is "Idle").
     * Used to reflect motor state in the UI (e.g., disable motion controls when motors off).
     */
    lv_subject_t* get_motors_enabled_subject() {
        return calibration_state_.get_motors_enabled_subject();
    }

    /**
     * @brief Check if printer has a probe configured
     *
     * Used by Z-offset calibration to determine whether to use
     * PROBE_CALIBRATE (has probe) or Z_ENDSTOP_CALIBRATE (no probe).
     *
     * @return true if [probe] or [bltouch] section exists in Klipper config
     */
    bool has_probe() {
        return capabilities_state_.has_probe();
    }

    /**
     * @brief Get the configured (saved) z-offset in microns
     *
     * Returns the printer's saved z-offset value before calibration started.
     * For probe printers: reads probe z_offset from ProbeSensorManager.
     * For endstop printers: reads stepper_z position_endstop from config.
     *
     * @return Z-offset in microns (e.g., -1500 for -1.500mm)
     */
    int get_configured_z_offset_microns();

    /**
     * @brief Set stepper_z position_endstop (for non-probe printers)
     *
     * Forwarded to PrinterCapabilitiesState.
     *
     * @param microns position_endstop in microns
     */
    void set_stepper_z_endstop_microns(int microns) {
        capabilities_state_.set_stepper_z_endstop_microns(microns);
    }

    // ========================================================================
    // HARDWARE VALIDATION API
    // ========================================================================

    /**
     * @brief Set hardware validation result and update subjects
     *
     * Updates all hardware validation subjects based on the validation result.
     * Call after HardwareValidator::validate() completes.
     *
     * @param result Validation result from HardwareValidator
     */
    void set_hardware_validation_result(const HardwareValidationResult& result);

    /**
     * @brief Get hardware has issues subject for UI binding
     *
     * Integer subject: 0=no issues, 1=has issues.
     * Use with bind_flag_if_eq to show/hide Hardware Health section.
     */
    lv_subject_t* get_hardware_has_issues_subject() {
        return hardware_validation_state_.get_hardware_has_issues_subject();
    }

    /**
     * @brief Get hardware issue count subject for UI binding
     *
     * Integer subject with total number of validation issues.
     */
    lv_subject_t* get_hardware_issue_count_subject() {
        return hardware_validation_state_.get_hardware_issue_count_subject();
    }

    /**
     * @brief Get hardware max severity subject for UI binding
     *
     * Integer subject: 0=info, 1=warning, 2=critical.
     * Use for styling (color) based on severity.
     */
    lv_subject_t* get_hardware_max_severity_subject() {
        return hardware_validation_state_.get_hardware_max_severity_subject();
    }

    /**
     * @brief Get hardware validation version subject
     *
     * Integer subject incremented when validation changes.
     * UI should observe to refresh dynamic lists.
     */
    lv_subject_t* get_hardware_validation_version_subject() {
        return hardware_validation_state_.get_hardware_validation_version_subject();
    }

    /**
     * @brief Get the hardware issues label subject
     *
     * String subject with formatted label like "1 Hardware Issue" or "5 Hardware Issues".
     * Used for settings panel row label binding.
     */
    lv_subject_t* get_hardware_issues_label_subject() {
        return hardware_validation_state_.get_hardware_issues_label_subject();
    }

    /**
     * @brief Check if hardware validation has any issues
     */
    bool has_hardware_issues() {
        return hardware_validation_state_.has_hardware_issues();
    }

    /**
     * @brief Get the stored hardware validation result
     *
     * Returns the most recent validation result set via set_hardware_validation_result().
     * Use this to access detailed issue information for UI display.
     *
     * @return Reference to the stored validation result
     */
    const HardwareValidationResult& get_hardware_validation_result() const {
        return hardware_validation_state_.get_hardware_validation_result();
    }

    /**
     * @brief Remove a hardware issue from the cached validation result
     *
     * Removes the issue matching the given hardware name from all issue lists
     * and updates all related subjects (counts, status text, etc.).
     * Used when user clicks "Ignore" or "Save" on a hardware issue.
     *
     * @param hardware_name The hardware name to remove (e.g., "filament_sensor runout")
     */
    void remove_hardware_issue(const std::string& hardware_name);

    // ========================================================================
    // PRINTER TYPE AND PRINT START CAPABILITIES
    // ========================================================================

    /**
     * @brief Set the printer type and fetch capabilities from database (async)
     *
     * Stores the type name and fetches PrintStartCapabilities from the
     * printer database via PrinterDetector::get_print_start_capabilities().
     *
     * Thread-safe: Uses helix::async::call_method_ref() to defer LVGL subject
     * updates to the main thread. Safe to call from WebSocket callbacks.
     *
     * @param type Printer type name (e.g., "FlashForge Adventurer 5M Pro")
     */
    void set_printer_type(const std::string& type);

    /**
     * @brief Set the printer type synchronously (main-thread only)
     *
     * Directly updates printer type without async deferral.
     * Only call this from the main LVGL thread (e.g., in tests with init_subjects(false)).
     *
     * @param type Printer type name (e.g., "FlashForge Adventurer 5M Pro")
     */
    void set_printer_type_sync(const std::string& type);

    /**
     * @brief Get the current printer type name
     *
     * @return Const reference to the stored printer type string
     */
    const std::string& get_printer_type() const;

    /**
     * @brief Get the print start capabilities for the current printer type
     *
     * Returns capabilities fetched from the database when set_printer_type()
     * was called. If the printer type is unknown or not set, returns an
     * empty capabilities struct.
     *
     * @return Const reference to the PrintStartCapabilities
     */
    const PrintStartCapabilities& get_print_start_capabilities() const;

    /**
     * @brief Get the Z-offset calibration strategy for this printer
     */
    ZOffsetCalibrationStrategy get_z_offset_calibration_strategy() const;

    // ========================================================================
    // MULTI-PRINTER SUBJECTS
    // ========================================================================

    /**
     * @brief Get the active printer display name subject
     *
     * String subject holding the human-readable name of the active printer.
     * Use with bind_text in XML to display the current printer name.
     */
    lv_subject_t* get_active_printer_name_subject() { return &active_printer_name_; }

    /**
     * @brief Get the multi-printer enabled subject
     *
     * Integer subject: 1 when multiple printers are configured, 0 otherwise.
     * Use with bind_flag_if_eq in XML to show/hide multi-printer UI elements.
     */
    lv_subject_t* get_multi_printer_enabled_subject() { return &multi_printer_enabled_; }

    /**
     * @brief Set the active printer display name
     *
     * Updates the string subject with the given name. Main-thread only.
     *
     * @param name Human-readable printer name
     */
    void set_active_printer_name(const std::string& name);

    /**
     * @brief Set whether multiple printers are configured
     *
     * Updates the integer subject. Main-thread only.
     *
     * @param enabled true if more than one printer is configured
     */
    void set_multi_printer_enabled(bool enabled);

  private:
    /// RAII manager for automatic subject cleanup - deinits all subjects on destruction
    SubjectManager subjects_;

    /// Temperature state component (extruder and bed temperatures)
    helix::PrinterTemperatureState temperature_state_;

    /// Motion state component (position, speed/flow, z-offset)
    helix::PrinterMotionState motion_state_;

    /// LED state component (RGBW channels, brightness, on/off state)
    helix::PrinterLedState led_state_component_;

    /// Fan state component (fan speed, multi-fan tracking)
    helix::PrinterFanState fan_state_;

    /// Print state component (progress, state, timing, layers, print start)
    helix::PrinterPrintState print_domain_;

    /// Capabilities state component (hardware capabilities, feature availability)
    helix::PrinterCapabilitiesState capabilities_state_;

    /// Plugin status component (helix_plugin_installed, phase_tracking_enabled)
    helix::PrinterPluginStatusState plugin_status_state_;

    /// Calibration state component (firmware retraction, manual probe, motor state)
    helix::PrinterCalibrationState calibration_state_;

    /// Hardware validation state component (issue counts, severity, status text)
    helix::PrinterHardwareValidationState hardware_validation_state_;

    /// Composite visibility state component (can_show_* derived subjects)
    helix::PrinterCompositeVisibilityState composite_visibility_state_;

    /// Network state component (connection, klippy, nav buttons)
    helix::PrinterNetworkState network_state_;

    /// Versions state component (klipper and moonraker version strings)
    helix::PrinterVersionsState versions_state_;

    /// Excluded objects state component (excluded_objects_version, excluded_objects set)
    helix::PrinterExcludedObjectsState excluded_objects_state_;

    // Note: Print subjects are now managed by print_domain_ component
    // (print_progress_, print_filename_, print_state_, print_state_enum_,
    //  print_outcome_, print_active_, print_show_progress_, print_display_filename_,
    //  print_thumbnail_path_, print_layer_current_, print_layer_total_,
    //  print_duration_, print_time_left_, print_start_phase_, print_start_message_,
    //  print_start_progress_, print_in_progress_)

    // Note: Motion subjects (position_x_, position_y_, position_z_, homed_axes_,
    // speed_factor_, flow_factor_, gcode_z_offset_, pending_z_offset_delta_)
    // are now managed by motion_state_ component

    // Note: Fan subjects (fan_speed_, fans_, fans_version_, fan_speed_subjects_)
    // are now managed by fan_state_ component

    // Note: Network subjects (printer_connection_state_, printer_connection_message_,
    // network_status_, klippy_state_, nav_buttons_enabled_, was_ever_connected_)
    // are now managed by network_state_ component

    // Note: LED subjects (led_state_, led_r_, led_g_, led_b_, led_w_, led_brightness_)
    // are now managed by led_state_component_

    // Note: Excluded objects subjects (excluded_objects_version_, excluded_objects_)
    // are now managed by excluded_objects_state_ component

    // Note: Printer capability subjects (printer_has_qgl_, printer_has_z_tilt_,
    // printer_has_bed_mesh_, printer_has_nozzle_clean_, printer_has_probe_,
    // printer_has_heater_bed_, printer_has_led_, printer_has_accelerometer_,
    // printer_has_spoolman_, printer_has_speaker_, printer_has_timelapse_,
    // printer_has_purge_line_, printer_has_firmware_retraction_, printer_bed_moves_)
    // are now managed by capabilities_state_ component

    // Note: Plugin status subjects (helix_plugin_installed_, phase_tracking_enabled_)
    // are now managed by plugin_status_state_ component

    // Note: Composite visibility subjects (can_show_bed_mesh_, can_show_qgl_,
    // can_show_z_tilt_, can_show_nozzle_clean_, can_show_purge_line_)
    // are now managed by composite_visibility_state_ component

    // Note: Firmware retraction, manual probe, and motor state subjects
    // (retract_length_, retract_speed_, unretract_extra_length_, unretract_speed_,
    //  manual_probe_active_, manual_probe_z_position_, motors_enabled_)
    // are now managed by calibration_state_ component

    // Note: Version subjects (klipper_version_, moonraker_version_) are now managed
    // by versions_state_ component

    // Note: Hardware validation subjects (hardware_has_issues_, hardware_issue_count_,
    // hardware_max_severity_, hardware_validation_version_, hardware_critical_count_,
    // hardware_warning_count_, hardware_info_count_, hardware_session_count_,
    // hardware_status_title_, hardware_status_detail_, hardware_issues_label_,
    // hardware_validation_result_) are now managed by hardware_validation_state_ component

    // Note: tracked_led_name_ is now managed by led_state_component_

    // Note: String buffers are now managed by their respective component classes
    // - homed_axes_buf_ is now in motion_state_ component
    // - print-related buffers are now in print_domain_ component
    // - hardware validation buffers are now in hardware_validation_state_ component
    // - printer_connection_message_buf_ is now in network_state_ component
    // - klipper_version_buf_, moonraker_version_buf_ are now in versions_state_ component

    // Multi-printer subjects (owned directly by PrinterState)
    lv_subject_t active_printer_name_;
    char active_printer_name_buf_[128];
    lv_subject_t multi_printer_enabled_;

    // JSON cache for complex data
    json json_state_;
    std::mutex state_mutex_;

    // Initialization guard to prevent multiple subject initializations
    bool subjects_initialized_ = false;

    // Cached display pointer to detect LVGL reinitialization (for test isolation)
    lv_display_t* cached_display_ = nullptr;

    // Note: was_ever_connected_ is now managed by network_state_ component

    // Capability override layer (user config overrides for auto-detected capabilities)
    CapabilityOverrides capability_overrides_;

    // Printer type and print start capabilities
    std::string printer_type_;                        ///< Selected printer type name
    PrintStartCapabilities print_start_capabilities_; ///< Cached capabilities for current type
    ZOffsetCalibrationStrategy z_offset_calibration_strategy_ =
        ZOffsetCalibrationStrategy::PROBE_CALIBRATE;

    /// Last kinematics string (to skip redundant recomputation)
    std::string last_kinematics_;

    /// Auto-detected bed_moves value from kinematics (before user override)
    bool auto_detected_bed_moves_ = false;

    // ============================================================================
    // Thread-safe internal methods (called via lv_async_call from main thread)
    // ============================================================================
    // These methods contain the actual LVGL subject updates and must only be called
    // from the main thread. The public methods (set_hardware, etc.) use
    // lv_async_call to defer to these internal methods, ensuring thread safety.

    friend class PrinterStateTestAccess;
    friend class PrinterTemperatureStateTestAccess;
    friend void async_klipper_version_callback(void* user_data);
    friend void async_moonraker_version_callback(void* user_data);
    friend void async_klippy_state_callback(void* user_data);

    void set_hardware_internal(const helix::PrinterDiscovery& hardware);
    void set_klipper_version_internal(const std::string& version);
    void set_moonraker_version_internal(const std::string& version);
    void set_os_version_internal(const std::string& version);
    void set_klippy_state_internal(KlippyState state);
    void set_printer_type_internal(const std::string& type);

    /**
     * @brief Update combined nav_buttons_enabled subject
     *
     * Recalculates nav_buttons_enabled based on connection and klippy state.
     * Called whenever printer_connection_state or klippy_state changes.
     */
    void update_nav_buttons_enabled();

    /**
     * @brief Update composite visibility subjects for G-code modification options
     *
     * Recalculates can_show_* subjects based on current plugin and capability state.
     * Called whenever helix_plugin_installed or printer_has_* subjects change.
     * Must be called from main thread (typically via async callbacks).
     */
    void update_gcode_modification_visibility();
};

} // namespace helix
