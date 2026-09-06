// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "async_lifetime_guard.h"
#include "capability_overrides.h"
#include "printer_discovery.h"
#include "subject_managed_panel.h"

#include <lvgl.h>
#include <unordered_map>

namespace helix {

/**
 * @brief Manages printer capability subjects for UI feature visibility
 *
 * Tracks hardware capabilities (probe, heater bed, LED, accelerometer, etc.)
 * and feature availability (spoolman, timelapse, firmware retraction, etc.)
 * Provides 18+ subjects for reactive UI updates based on printer capabilities.
 * Extracted from PrinterState as part of god class decomposition.
 *
 * @note Capability values are set from hardware discovery on connect, with
 *       user overrides applied from CapabilityOverrides. Some capabilities
 *       (spoolman, purge_line, bed_moves) are updated asynchronously.
 */
class PrinterCapabilitiesState {
  public:
    PrinterCapabilitiesState() = default;
    ~PrinterCapabilitiesState() = default;

    // Non-copyable
    PrinterCapabilitiesState(const PrinterCapabilitiesState&) = delete;
    PrinterCapabilitiesState& operator=(const PrinterCapabilitiesState&) = delete;

    /**
     * @brief Initialize capability subjects
     * @param register_xml If true, register subjects with LVGL XML system
     */
    void init_subjects(bool register_xml = true);

    /**
     * @brief Deinitialize subjects (called by SubjectManager automatically)
     */
    void deinit_subjects();

    // ========================================================================
    // Hardware update methods
    // ========================================================================

    /**
     * @brief Update capabilities from hardware discovery with overrides applied
     *
     * Called from PrinterState::set_hardware() when hardware is detected.
     * Uses effective values from capability_overrides (auto-detect + user overrides).
     *
     * @param hardware Auto-detected hardware capabilities
     * @param overrides Capability override layer with effective values
     */
    void set_hardware(const PrinterDiscovery& hardware, const CapabilityOverrides& overrides);

    /**
     * @brief Set spoolman availability (async update from Moonraker query)
     *
     * Thread-safe: Uses helix::ui::queue_update() for main-thread execution.
     *
     * @param available True if spoolman is available
     */
    void set_spoolman_available(bool available);

    /**
     * @brief Set purge line capability (from printer type database)
     *
     * Called when printer type is set to update has_purge_line based on
     * printer-specific capabilities.
     *
     * @param has_purge_line True if printer has purge/priming capability
     */
    void set_purge_line(bool has_purge_line);

    /**
     * @brief Set webcam availability (async update from Moonraker query)
     *
     * Thread-safe: Uses helix::ui::queue_update() for main-thread execution.
     *
     * @param available True if at least one enabled webcam is configured
     * @param stream_url MJPEG stream URL of first enabled webcam (empty if none)
     * @param snapshot_url Snapshot URL of first enabled webcam (empty if none)
     */
    void set_webcam_available(bool available, const std::string& stream_url = "",
                              const std::string& snapshot_url = "", bool flip_h = false,
                              bool flip_v = false, int target_fps = 15);

    /**
     * @brief Set timelapse plugin availability (async update)
     *
     * Thread-safe: Uses helix::ui::queue_update() for main-thread execution.
     *
     * @param available True if moonraker-timelapse plugin is installed and responding
     */
    void set_timelapse_available(bool available);

    /**
     * @brief Set bed moves on Z axis (from kinematics detection)
     *
     * @param bed_moves True if bed moves on Z (corexy), false if gantry moves (cartesian/delta)
     */
    void set_bed_moves(bool bed_moves);

    /** @brief Override chamber sensor capability after manual assignment resolution */
    void set_has_chamber_sensor(bool available);

    /** @brief Override chamber heater capability after manual assignment resolution */
    void set_has_chamber_heater(bool available);

    /** @brief Set chamber-heater diagnostics capability (backend diagnostics object resolved) */
    void set_has_chamber_heater_diagnostics(bool available);

    /** @brief Set chamber filter-fan capability (backend filter pin resolved) */
    void set_has_chamber_filter_fan(bool available);

    /**
     * @brief Set stepper_z position_endstop value (for non-probe printers)
     *
     * Stores the configured position_endstop from stepper_z in Klipper's
     * configfile.settings. Used as the "saved z-offset" reference for
     * endstop-based printers during Z-offset calibration.
     *
     * @param microns position_endstop in microns (e.g., 235000 for 235.0mm)
     */
    void set_stepper_z_endstop_microns(int microns) {
        stepper_z_endstop_microns_ = microns;
    }

    /**
     * @brief Get stepper_z position_endstop value in microns
     *
     * @return position_endstop in microns, or 0 if not set
     */
    int get_stepper_z_endstop_microns() const {
        return stepper_z_endstop_microns_;
    }

    /**
     * @brief Set power device count (async update from Moonraker query)
     *
     * Thread-safe: Uses helix::ui::queue_update() for main-thread execution.
     *
     * @param count Number of discovered power devices
     */
    void set_power_device_count(int count);

    /**
     * @brief Set Moonraker sensor count (async update from discovery)
     *
     * Thread-safe: Uses helix::ui::queue_update() for main-thread execution.
     *
     * @param count Number of discovered Moonraker sensors
     */
    void set_sensor_count(int count);

    // ========================================================================
    // Subject accessors
    // ========================================================================

    /// 1 if printer has quad_gantry_level
    lv_subject_t* get_printer_has_qgl_subject() const {
        return const_cast<lv_subject_t*>(&printer_has_qgl_);
    }

    /// 1 if printer has z_tilt_adjust
    lv_subject_t* get_printer_has_z_tilt_subject() const {
        return const_cast<lv_subject_t*>(&printer_has_z_tilt_);
    }

    /// 1 if printer has bed_mesh calibration
    lv_subject_t* get_printer_has_bed_mesh_subject() const {
        return const_cast<lv_subject_t*>(&printer_has_bed_mesh_);
    }

    /// 1 if printer has nozzle clean macro
    lv_subject_t* get_printer_has_nozzle_clean_subject() const {
        return const_cast<lv_subject_t*>(&printer_has_nozzle_clean_);
    }

    /// 1 if printer has probe or bltouch
    lv_subject_t* get_printer_has_probe_subject() const {
        return const_cast<lv_subject_t*>(&printer_has_probe_);
    }

    /// 1 if printer has heated bed
    lv_subject_t* get_printer_has_heater_bed_subject() const {
        return const_cast<lv_subject_t*>(&printer_has_heater_bed_);
    }

    /// 1 if printer has controllable LED
    lv_subject_t* get_printer_has_led_subject() const {
        return const_cast<lv_subject_t*>(&printer_has_led_);
    }

    /// 1 if printer has accelerometer for input shaping
    lv_subject_t* get_printer_has_accelerometer_subject() const {
        return const_cast<lv_subject_t*>(&printer_has_accelerometer_);
    }

    /// 1 if spoolman filament manager is available
    lv_subject_t* get_printer_has_spoolman_subject() const {
        return const_cast<lv_subject_t*>(&printer_has_spoolman_);
    }

    /// 1 if printer has speaker for M300 audio
    lv_subject_t* get_printer_has_speaker_subject() const {
        return const_cast<lv_subject_t*>(&printer_has_speaker_);
    }

    /**
     * @brief Set speaker availability from local sound backend detection.
     *
     * Called after SoundManager initializes to ensure sound settings are visible
     * even before hardware discovery completes (or when Klipper is not connected).
     * set_hardware() may later update this based on full capability evaluation.
     */
    void set_sound_backend_available(bool available);

    /// 1 if moonraker-timelapse plugin is installed
    lv_subject_t* get_printer_has_timelapse_subject() const {
        return const_cast<lv_subject_t*>(&printer_has_timelapse_);
    }

    /// 1 if printer has purge/priming capability
    lv_subject_t* get_printer_has_purge_line_subject() const {
        return const_cast<lv_subject_t*>(&printer_has_purge_line_);
    }

    /// 1 if printer has firmware retraction (G10/G11)
    lv_subject_t* get_printer_has_firmware_retraction_subject() const {
        return const_cast<lv_subject_t*>(&printer_has_firmware_retraction_);
    }

    /// 1 if bed moves on Z axis, 0 if gantry moves
    lv_subject_t* get_printer_bed_moves_subject() const {
        return const_cast<lv_subject_t*>(&printer_bed_moves_);
    }

    /// 1 if printer has chamber temperature sensor
    lv_subject_t* get_printer_has_chamber_sensor_subject() const {
        return const_cast<lv_subject_t*>(&printer_has_chamber_sensor_);
    }

    /// 1 if printer has active chamber heater (heater_generic chamber)
    lv_subject_t* get_printer_has_chamber_heater_subject() const {
        return const_cast<lv_subject_t*>(&printer_has_chamber_heater_);
    }

    /// 1 if the chamber heater exposes backend diagnostics
    lv_subject_t* get_printer_has_chamber_heater_diagnostics_subject() const {
        return const_cast<lv_subject_t*>(&printer_has_chamber_heater_diagnostics_);
    }

    /// 1 if the chamber has a backend-resolved filter fan (output_pin)
    lv_subject_t* get_printer_has_chamber_filter_fan_subject() const {
        return const_cast<lv_subject_t*>(&printer_has_chamber_filter_fan_);
    }

    /// 1 if printer has any chamber capability (sensor OR heater)
    lv_subject_t* get_printer_has_chamber_subject() const {
        return const_cast<lv_subject_t*>(&printer_has_chamber_);
    }

    /// 1 if printer has screws_tilt_adjust
    lv_subject_t* get_printer_has_screws_tilt_subject() const {
        return const_cast<lv_subject_t*>(&printer_has_screws_tilt_);
    }

    /// 1 if the printer can calibrate its tool offsets automatically (a tool
    /// changer with the CALIBRATE_TOOL_OFFSETS macro, see
    /// include/tool_offset_calibration.h)
    lv_subject_t* get_printer_has_tool_offset_cal_subject() const {
        return const_cast<lv_subject_t*>(&printer_has_tool_offset_cal_);
    }

    /// 1 if printer has an enabled webcam configured
    lv_subject_t* get_printer_has_webcam_subject() const {
        return const_cast<lv_subject_t*>(&printer_has_webcam_);
    }

    /// MJPEG stream URL of first enabled webcam (empty if none)
    const std::string& get_webcam_stream_url() const {
        return webcam_stream_url_;
    }

    /// Snapshot URL of first enabled webcam (empty if none)
    const std::string& get_webcam_snapshot_url() const {
        return webcam_snapshot_url_;
    }

    /// Webcam flip flags from Moonraker config
    bool get_webcam_flip_horizontal() const {
        return webcam_flip_h_;
    }
    bool get_webcam_flip_vertical() const {
        return webcam_flip_v_;
    }

    /// Configured target FPS from Moonraker webcam config (default 15)
    int get_webcam_target_fps() const {
        return webcam_target_fps_;
    }

    /// 1 if printer has controllable fans beyond part cooling (generic fans, exhaust, etc.)
    lv_subject_t* get_printer_has_extra_fans_subject() const {
        return const_cast<lv_subject_t*>(&printer_has_extra_fans_);
    }

    /// Number of configured power devices (0 = none)
    lv_subject_t* get_power_device_count_subject() const {
        return const_cast<lv_subject_t*>(&power_device_count_);
    }

    /// Number of discovered Moonraker sensors (0 = none)
    lv_subject_t* get_sensor_count_subject() const {
        return const_cast<lv_subject_t*>(&sensor_count_);
    }

    // ========================================================================
    // Convenience methods
    // ========================================================================

    /**
     * @brief Check if printer has a probe
     * @return true if [probe] or [bltouch] section exists in Klipper config
     */
    bool has_probe() const {
        // Cast away const for lv_subject_get_int which doesn't modify the subject
        return lv_subject_get_int(const_cast<lv_subject_t*>(&printer_has_probe_)) != 0;
    }

  private:
    friend class PrinterCapabilitiesStateTestAccess;

    /// Update combined printer_has_chamber_ from sensor and heater flags
    void update_has_chamber();

    SubjectManager subjects_;
    bool subjects_initialized_ = false;

    /// Capability answers that arrived before the subjects existed.
    ///
    /// Discovery runs on the WebSocket thread and its answers reach here through
    /// AsyncLifetimeGuard::defer(), so on a fast printer they can land before
    /// init_subjects() has run. Writing an uninitialised lv_subject_t is already
    /// wrong, and INIT_SUBJECT_INT would then reset it to the hardcoded default
    /// anyway - the answer is simply lost, and nothing re-runs discovery in a
    /// stable session. That is how a connected Spoolman stayed dark for five days
    /// on a K2 Plus (2026-08-24): SpoolmanManager's observer watches this flag's
    /// falling edge and drops the identity cache behind every slot's vendor and
    /// material. Latch instead, and seed init_subjects() from what we already know.
    std::unordered_map<lv_subject_t*, int> pending_capability_values_;

    /// Write a capability subject, or latch the value when subjects do not exist.
    void set_capability_int(lv_subject_t& subject, int value);

    /// Replay everything latched before init_subjects() ran.
    void apply_pending_capability_values();

    /// Generation guard for the async setters that defer their subject writes to
    /// the main thread. Invalidated by `deinit_subjects()` and by destruction, so
    /// a callback still queued when the subjects go away is dropped instead of
    /// notifying a freed observer list (#1165, #1146). Declared after
    /// `subjects_` so it is destroyed first, ahead of the SubjectManager that
    /// deinits what those callbacks write.
    AsyncLifetimeGuard async_lifetime_;

    /// stepper_z position_endstop from configfile.settings (microns)
    int stepper_z_endstop_microns_ = 0;

    // Printer capability subjects (all integer: 0=no, 1=yes)
    lv_subject_t printer_has_qgl_{};                 // quad_gantry_level
    lv_subject_t printer_has_z_tilt_{};              // z_tilt_adjust
    lv_subject_t printer_has_bed_mesh_{};            // bed_mesh calibration
    lv_subject_t printer_has_nozzle_clean_{};        // nozzle clean macro
    lv_subject_t printer_has_probe_{};               // probe or bltouch
    lv_subject_t printer_has_heater_bed_{};          // heated bed
    lv_subject_t printer_has_led_{};                 // controllable LED
    lv_subject_t printer_has_accelerometer_{};       // accelerometer for input shaping
    lv_subject_t printer_has_spoolman_{};            // spoolman filament manager
    lv_subject_t printer_has_speaker_{};             // speaker for M300
    lv_subject_t printer_has_timelapse_{};           // moonraker-timelapse plugin
    lv_subject_t printer_has_tool_offset_cal_{};     // automatic tool offset calibration
    lv_subject_t printer_has_purge_line_{};          // purge/priming capability
    lv_subject_t printer_has_firmware_retraction_{}; // firmware retraction (G10/G11)
    lv_subject_t printer_bed_moves_{};               // 0=gantry moves on Z, 1=bed moves on Z
    lv_subject_t printer_has_chamber_sensor_{};      // chamber temperature sensor
    lv_subject_t printer_has_chamber_heater_{};      // active chamber heater (heater_generic)
    lv_subject_t
        printer_has_chamber_heater_diagnostics_{};  // chamber heater exposes backend diagnostics
    lv_subject_t printer_has_chamber_filter_fan_{}; // chamber filter fan (output_pin)
    lv_subject_t printer_has_chamber_{};            // combined: sensor OR heater
    lv_subject_t printer_has_screws_tilt_{};        // screws_tilt_adjust
    lv_subject_t printer_has_webcam_{};             // enabled webcam configured
    std::string webcam_stream_url_;                 // MJPEG stream URL
    std::string webcam_snapshot_url_;               // snapshot URL
    bool webcam_flip_h_ = false;                    // flip horizontal
    bool webcam_flip_v_ = false;                    // flip vertical
    int webcam_target_fps_ = 15;                    // configured target FPS
    lv_subject_t printer_has_extra_fans_{};         // extra controllable fans beyond part cooling
    lv_subject_t power_device_count_{};             // number of power devices (0 = none)
    lv_subject_t sensor_count_{};                   // number of Moonraker sensors (0 = none)
};

} // namespace helix
