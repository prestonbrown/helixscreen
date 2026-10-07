// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "async_lifetime_guard.h"
#include "capability_overrides.h"
#include "moonraker_types.h"
#include "printer_discovery.h"
#include "subject_managed_panel.h"

#include <array>
#include <cstdint>
#include <lvgl.h>
#include <optional>
#include <string>
#include <vector>

namespace helix {

/// Every integer capability subject. XML names and defaults live in the table
/// in printer_capabilities_state.cpp, in this order.
enum class Capability : uint8_t {
    HasQgl,                 ///< quad_gantry_level
    HasZTilt,               ///< z_tilt_adjust
    HasBedMesh,             ///< bed_mesh calibration
    HasNozzleClean,         ///< nozzle clean macro
    HasProbe,               ///< probe or bltouch
    HasHeaterBed,           ///< heated bed
    HasLed,                 ///< controllable LED
    HasAccelerometer,       ///< accelerometer for input shaping
    HasSpoolman,            ///< Spoolman reachable
    HasSpeaker,             ///< M300 beeper or a local sound backend
    HasTimelapse,           ///< moonraker-timelapse
    HasJobQueue,            ///< Moonraker job_queue component
    HasPurgeLine,           ///< purge/priming capability
    HasFirmwareRetraction,  ///< firmware retraction (G10/G11)
    HasIndividualXyzHoming, ///< 0 on deltas: every axis homes together
    SupportsBeltCompare,    ///< corexy/limited_corexy: the two diagonals are the two belt paths
    BedMoves,               ///< 0 = gantry moves on Z, 1 = bed moves on Z
    IsEnclosed,             ///< enclosure, after the user override
    CanBedDry,              ///< drying on the heated bed is offered: heated bed, enclosed, enough Z
    HasChamberSensor,       ///< chamber temperature sensor
    HasChamberHeater,       ///< active chamber heater
    HasChamberHeaterDiagnostics, ///< chamber heater exposes backend diagnostics
    HasChamberFilterFan,         ///< chamber filter fan (output_pin)
    HasChamberElementTemp,       ///< backend reports the heating element's temperature
    HasChamberDryer,             ///< backend runs a filament-drying cycle
    HasChamber,                  ///< chamber sensor OR heater
    HasScrewsTilt,               ///< screws_tilt_adjust
    HasExcludeObject,            ///< [exclude_object]
    HasToolOffsetCal,            ///< automatic tool offset calibration
    HideManualZCalibration,      ///< tool offset calibration also sets the reference tool's Z
    HasPaCal,                    ///< firmware measures pressure advance
    HasWebcam,                   ///< an enabled webcam is configured
    WebcamCount,                 ///< named webcams (what a picker can offer)
    HasExtraFans,                ///< controllable fans beyond part cooling
    PowerDeviceCount,            ///< power devices (0 = none)
    SensorCount,                 ///< Moonraker sensors (0 = none)
    Count
};

inline constexpr size_t CAPABILITY_COUNT = static_cast<size_t>(Capability::Count);

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
     * @brief Set whether the manual Z calibration (paper test) is hidden
     * (from printer type database)
     *
     * Called when printer type is set. True only where the database says the
     * CALIBRATE_TOOL_OFFSETS routine sets every tool's Z, the reference tool
     * included, so the paper test has nothing left to set.
     *
     * @param hide True if the paper test is redundant on this printer
     */
    void set_hide_manual_z_calibration(bool hide);

    /**
     * @brief Publish the printer's webcam list (async update from discovery)
     *
     * Keeps every entry so a camera view configured with a `source` name can
     * find its camera (webcam::select_webcam), and derives the auto-pick
     * (webcam::auto_pick) into the single-feed getters below, which every
     * caller with no preference reads. `printer_has_webcam` follows the
     * auto-pick; `webcam_count` is the number of NAMED entries — the ones a
     * picker can offer.
     *
     * Thread-safe: Uses helix::ui::queue_update() for main-thread execution.
     */
    void set_webcams(std::vector<WebcamInfo> cams);

    /**
     * @brief Publish a single feed by URL, or no feed at all
     *
     * The one-entry form of set_webcams() for callers that have URLs rather
     * than a Moonraker list (the mock client, tests). A non-empty stream_url
     * is taken to be MJPEG — the caller vouches for it — so the auto-pick
     * streams it instead of polling the snapshot.
     *
     * Thread-safe: Uses helix::ui::queue_update() for main-thread execution.
     *
     * @param available False publishes an empty list
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
     * @brief Set job queue availability (async update from Moonraker's
     * server.info components list)
     *
     * Thread-safe: Uses helix::ui::queue_update() for main-thread execution.
     *
     * @param available True if Moonraker lists the job_queue component
     */
    void set_job_queue_available(bool available);

    /**
     * @brief Set has individual XYZ homing (from kinematics detection)
     *
     * @param has_individual_xyz_homing True if XYZ axes can be homed individually,
     * false otherwise (delta/rotary_delta)
     */
    void set_has_individual_xyz_homing(bool has_individual_xyz_homing);

    /// 1 when the kinematics is a CoreXY whose two diagonals are the two belt
    /// paths (corexy, limited_corexy) - what Belt Tension measures. Set from
    /// PrinterState::set_kinematics.
    void set_supports_belt_compare(bool supports);

    /**
     * @brief Set bed moves on Z axis (from kinematics detection)
     *
     * @param bed_moves True if bed moves on Z (corexy), false if gantry moves (cartesian/delta)
     */
    void set_bed_moves(bool bed_moves);

    /// Enclosed printer, and whether drying on the heated bed is offered
    /// (prestonbrown/helixscreen#1730). Resolved by PrinterState.
    void set_bed_drying(bool enclosed, bool can_bed_dry);

    /** @brief Override chamber sensor capability after manual assignment resolution */
    void set_has_chamber_sensor(bool available);

    /** @brief Override chamber heater capability after manual assignment resolution */
    void set_has_chamber_heater(bool available);

    /** @brief Set chamber-heater diagnostics capability (backend diagnostics object resolved) */
    void set_has_chamber_heater_diagnostics(bool available);

    /** @brief Set chamber filter-fan capability (backend filter pin resolved) */
    void set_has_chamber_filter_fan(bool available);

    /** @brief Set chamber element-temperature capability (backend reports one) */
    void set_has_chamber_element_temp(bool available);

    /** @brief Set chamber filament-dryer capability (backend has a drying cycle) */
    void set_has_chamber_dryer(bool available);

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

    /// The subject for one capability. Valid once init_subjects() has run.
    lv_subject_t* subject(Capability cap) const {
        return const_cast<lv_subject_t*>(&capability_subjects_[static_cast<size_t>(cap)]);
    }

    /**
     * @brief Set speaker availability from local sound backend detection.
     *
     * Called after SoundManager initializes to ensure sound settings are visible
     * even before hardware discovery completes (or when Klipper is not connected).
     * set_hardware() may later update this based on full capability evaluation.
     */
    void set_sound_backend_available(bool available);

    /// Every enabled webcam discovery found, in Moonraker's order. Main thread only.
    const std::vector<WebcamInfo>& get_webcams() const {
        return webcams_;
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

    // ========================================================================
    // Convenience methods
    // ========================================================================

    /**
     * @brief Check if printer has a probe
     * @return true if [probe] or [bltouch] section exists in Klipper config
     */
    bool has_probe() const {
        return capability_value(Capability::HasProbe) != 0;
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
    std::array<std::optional<int>, CAPABILITY_COUNT> pending_capability_values_{};

    /// Write a capability subject, or latch the value when subjects do not exist.
    void set_capability(Capability cap, int value);

    /// The subject's value, or before init the latched (else default) value.
    int capability_value(Capability cap) const;

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

    std::array<lv_subject_t, CAPABILITY_COUNT> capability_subjects_{};
    std::vector<WebcamInfo> webcams_; // every enabled webcam, Moonraker order
    std::string webcam_stream_url_;   // auto-pick: MJPEG stream URL
    std::string webcam_snapshot_url_; // snapshot URL
    bool webcam_flip_h_ = false;      // flip horizontal
    bool webcam_flip_v_ = false;      // flip vertical
    int webcam_target_fps_ = 15;      // configured target FPS
};

} // namespace helix
