// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_heater_config.h" // helix::HeaterType

#include "app_macro_activity.h"
#include "app_motion_activity.h"
#include "async_lifetime_guard.h"
#include "busy_episode_owner.h"
#include "capability_overrides.h"
#include "hardware_validator.h"
#include "klippy_state.h"
#include "lvgl/lvgl.h"
#include "print_start_phase.h"
#include "printer_calibration_state.h"
#include "printer_capabilities_state.h"
#include "printer_composite_visibility_state.h"
#include "printer_detector.h"
#include "printer_discovery.h"
#include "printer_excluded_objects_state.h"
#include "printer_fan_state.h"
#include "printer_hardware_validation_state.h"
#include "printer_motion_state.h"
#include "printer_network_state.h"
#include "printer_plugin_status_state.h"
#include "printer_print_state.h"
#include "printer_profile_state.h"
#include "printer_temperature_state.h"
#include "printer_versions_state.h"
#include "spdlog/spdlog.h"
#include "state/subject_macros.h"
#include "subject_managed_panel.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
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
 * @brief Has the printer taken a job?
 *
 * True for PRINTING and PAUSED: the two wire states reachable only from a job
 * Klipper has accepted. A pause is not a lesser form of idle - the job exists,
 * its clock runs, and `CANCEL_PRINT`/`PAUSE` land on something real.
 *
 * This is the wire-level half of the pair. Ask it when the decision turns on
 * what the PRINTER reports: whether a macro would reach a live job, whether a
 * sibling field like `print_filename` describes a running print, whether a
 * transition to a terminal state ended a real one.
 *
 * @warning NOT the same question as `job_holds_machine(PrintState)`, which also
 *          counts `Preparing`. Preparing is a job the app has committed to and
 *          the printer has not reported, so during a host-side pre-start block
 *          this predicate is false while the toolhead is genuinely moving. Ask
 *          `job_holds_machine()` for "would acting now fight the printer", and
 *          this one for "does the printer hold a job". Several callers depend on
 *          the narrower answer and say so at their call site.
 *
 * The 0/1 subject mirror of this predicate is `print_active`, set from
 * `PrinterPrintState::status_indicates_active_print()`, which asks the same
 * question of a raw status payload and is defined in terms of this function.
 */
constexpr bool printer_has_job(PrintJobState state) {
    return state == PrintJobState::PRINTING || state == PrintJobState::PAUSED;
}

/// True when the printer reports how its last job ended: complete, cancelled or error.
constexpr bool job_has_ended(PrintJobState state) {
    return state == PrintJobState::COMPLETE || state == PrintJobState::CANCELLED ||
           state == PrintJobState::ERROR;
}

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
 * @brief Convert PrintJobState enum to display string
 *
 * Returns a human-readable string for UI display.
 *
 * @param state PrintJobState enum value
 * @return Display string (e.g., "Printing", "Paused")
 */
const char* print_job_state_to_string(PrintJobState state);

/**
 * @brief Whether a kinematics string names a round-bed machine
 *
 * The value is PrinterDiscovery::kinematics(), read from
 * configfile.config.printer.kinematics because toolhead.kinematics comes
 * back null in the status payload.
 */
constexpr bool circular_bed_kinematics(std::string_view kinematics) {
    return kinematics == "delta" || kinematics == "rotary_delta";
}

/// Whether a kinematics string is a CoreXY whose two diagonals are the two belt
/// paths, which is what the Belt Tension comparison measures. CoreXZ,
/// hybrid_corexy/hybrid_corexz, cartesian, delta and the rest are not.
constexpr bool belt_path_kinematics(std::string_view kinematics) {
    return kinematics == "corexy" || kinematics == "limited_corexy";
}

/// One notify_status_update, unpacked. `status` points into the notification.
struct StatusFrame {
    const json* status = nullptr; ///< params[0], the printer objects that changed
    double eventtime = 0.0;       ///< params[1]; 0 when the frame was synthesized
    bool from_cached_snapshot = false;
    bool whole_objects = false; ///< every field of each object, not only the changed ones
};

/**
 * @brief Unpack a Moonraker notification into the frame update_from_status() takes
 *
 * params[1] is Klipper's eventtime. It is monotonic-clock derived, so it survives
 * a Klipper restart and only rewinds on a host reboot: a usable freshness key
 * within one connection. kCachedSnapshotMarker says the frame is a replay of an
 * earlier snapshot rather than current traffic; kWholeObjectsMarker says it is a
 * subscription response rather than a delta.
 *
 * @return The frame, or nullopt for anything that is not a notify_status_update
 */
std::optional<StatusFrame> parse_status_notification(const json& notification);
/// The frame points into the notification, so a temporary would leave it dangling.
std::optional<StatusFrame> parse_status_notification(json&&) = delete;

/**
 * @brief Printer state manager with LVGL 9 reactive subjects
 *
 * Implements hybrid architecture:
 * - LVGL subjects for UI-bound data (automatic reactive updates)
 * - JSON cache for complex data (file lists, capabilities, metadata)
 *
 * @note Thread Safety: Public setters that update LVGL subjects (set_klipper_version,
 *       set_printer_connection_state, etc.) use queue_update internally to defer updates to the
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
     * @brief Check if subjects have been initialized
     */
    bool are_subjects_initialized() const {
        return subjects_initialized_;
    }

    /**
     * @brief Deinitialize all subjects across all state components
     *
     * Cascades to all 13 sub-component deinit_subjects() methods and
     * then deinitializes PrinterState's own subjects.
     */
    void deinit_subjects();

    /**
     * @brief Update state from raw status data
     *
     * Updates subjects from a printer status object. Can be called directly
     * with subscription response data or extracted from notifications.
     * This is the core update logic used by both initial state and notifications.
     *
     * @param status Printer status object (e.g., from result.status or params[0])
     * @param eventtime Klipper's monotonic event timestamp (notification params[1]).
     *        0.0 means "no timestamp" — a synthesized status rather than a Klipper
     *        frame. Only the klippy-state freshness guard reads it; every other
     *        field is still last-write-wins.
     * @param from_cached_snapshot true when this payload was captured earlier and is
     *        being replayed (the discovery subscription response). Provenance is
     *        STATED, never inferred from a zero eventtime — the mock client also
     *        dispatches untimestamped status, and those ARE current.
     * @param frame_epoch klippy connection epoch the frame belongs to, when known
     * @param whole_objects true when this payload is a subscription response,
     *        carrying every field of each object rather than only the changed ones
     */
    void update_from_status(const json& status, double eventtime = 0.0,
                            bool from_cached_snapshot = false,
                            std::optional<uint64_t> frame_epoch = std::nullopt,
                            bool whole_objects = false);

    /**
     * @brief Apply a webhooks-shaped klippy status ({state, state_message}), main thread only
     *
     * The one path from a reported Klipper state to the klippy subjects: status
     * frames' webhooks object and a printer.info result both land here. Arguments
     * as update_from_status().
     */
    void apply_klippy_status(const json& webhooks, double eventtime = 0.0,
                             bool from_cached_snapshot = false,
                             std::optional<uint64_t> frame_epoch = std::nullopt);

    //
    // Domain components. Each owns its subjects and the state behind them;
    // reach a domain's subjects and queries through its accessor. Setters the
    // WebSocket thread calls stay on PrinterState, which defers them to the
    // main thread.
    //
    helix::PrinterTemperatureState& temperature_state() {
        return temperature_state_;
    }
    const helix::PrinterTemperatureState& temperature_state() const {
        return temperature_state_;
    }
    helix::PrinterMotionState& motion_state() {
        return motion_state_;
    }
    const helix::PrinterMotionState& motion_state() const {
        return motion_state_;
    }
    helix::PrinterFanState& fan_state() {
        return fan_state_;
    }
    const helix::PrinterFanState& fan_state() const {
        return fan_state_;
    }
    helix::PrinterPrintState& print_state() {
        return print_domain_;
    }
    const helix::PrinterPrintState& print_state() const {
        return print_domain_;
    }
    helix::PrinterCapabilitiesState& capabilities_state() {
        return capabilities_state_;
    }
    const helix::PrinterCapabilitiesState& capabilities_state() const {
        return capabilities_state_;
    }
    helix::PrinterPluginStatusState& plugin_status_state() {
        return plugin_status_state_;
    }
    const helix::PrinterPluginStatusState& plugin_status_state() const {
        return plugin_status_state_;
    }
    helix::PrinterCalibrationState& calibration_state() {
        return calibration_state_;
    }
    const helix::PrinterCalibrationState& calibration_state() const {
        return calibration_state_;
    }
    helix::PrinterHardwareValidationState& hardware_validation_state() {
        return hardware_validation_state_;
    }
    const helix::PrinterHardwareValidationState& hardware_validation_state() const {
        return hardware_validation_state_;
    }
    helix::PrinterCompositeVisibilityState& composite_visibility_state() {
        return composite_visibility_state_;
    }
    const helix::PrinterCompositeVisibilityState& composite_visibility_state() const {
        return composite_visibility_state_;
    }
    helix::PrinterNetworkState& network_state() {
        return network_state_;
    }
    const helix::PrinterNetworkState& network_state() const {
        return network_state_;
    }
    helix::PrinterVersionsState& versions_state() {
        return versions_state_;
    }
    const helix::PrinterVersionsState& versions_state() const {
        return versions_state_;
    }
    helix::PrinterExcludedObjectsState& excluded_objects_state() {
        return excluded_objects_state_;
    }
    const helix::PrinterExcludedObjectsState& excluded_objects_state() const {
        return excluded_objects_state_;
    }
    helix::PrinterProfileState& profile_state() {
        return profile_state_;
    }
    const helix::PrinterProfileState& profile_state() const {
        return profile_state_;
    }

    /// Duty for one heater, so every surface renders the same number rather
    /// than each mapping heater type to subject on its own.
    lv_subject_t* get_heater_power_subject(helix::HeaterType type) {
        switch (type) {
        case helix::HeaterType::Bed:
            return temperature_state_.get_bed_power_subject();
        case helix::HeaterType::Chamber:
            return temperature_state_.get_chamber_power_subject();
        case helix::HeaterType::Nozzle:
        default:
            return temperature_state_.get_extruder_power_subject();
        }
    }

    /// Re-format the text this state translates as it discovers hardware
    /// (extruder and fan names, hardware-health texts) in the current language.
    void refresh_translated_texts() {
        temperature_state_.refresh_display_names();
        fan_state_.refresh_display_names();
        hardware_validation_state_.refresh_texts();
    }

    /**
     * @brief Lifetime token covering EVERY subject reachable through PrinterState.
     *
     * The per-domain tokens above are narrower — one component's subjects, or a
     * single dynamic subject. This one is flipped false at the top of
     * `deinit_subjects()`, before any component tears its subjects down, so it
     * covers the whole tree: temperature, motion, fan, LED, print, capabilities,
     * excluded objects, and PrinterState's own.
     *
     * Any observer whose owner can outlive a `deinit_subjects()` cycle MUST pass
     * this (or a narrower token for the same subject) to `observe_*`. Panels held
     * in process-lifetime singletons are exactly that case: `deinit_subjects()`
     * runs `lv_subject_deinit()`, which frees every observer node, and an
     * ObserverGuard that never learned the subject died then calls
     * `lv_observer_remove()` on freed memory the next time it resets.
     *
     * Prefer the narrower per-subject token where one exists (dynamic per-fan and
     * per-extruder subjects die independently of a full deinit); use this for the
     * static subjects that only die with the whole PrinterState.
     */
    [[nodiscard]] SubjectLifetime get_subjects_lifetime() const {
        return subjects_.get_subjects_lifetime();
    }

    /**
     * @brief True when Klipper's pause_resume.is_paused is set.
     *
     * Reflects the last received pause_resume.is_paused value from Moonraker.
     * Safe to read synchronously from the main thread.
     */
    [[nodiscard]] bool is_paused() const {
        return is_paused_;
    }

    /**
     * @brief Firmware-persisted Z-offset in microns, or nullopt when unknown
     *
     * Convenience wrapper over the motion domain's persisted z-offset subjects for the
     * display/adjust helpers in helix::zoffset.
     */
    std::optional<int> get_persisted_z_offset_microns() {
        if (lv_subject_get_int(motion_state_.get_persisted_z_offset_valid_subject()) == 0) {
            return std::nullopt;
        }
        return lv_subject_get_int(motion_state_.get_persisted_z_offset_subject());
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

    /// Remote-screen verdict from the live websocket endpoint (thread-safe;
    /// defers the subject write to the main thread). Published by
    /// MoonrakerManager on CONNECTED edges.
    void set_moonraker_is_remote(bool remote);

    /// Main-thread read of moonraker_is_remote (true = connected Moonraker is
    /// not this host). For UI decision points; background code uses
    /// helix::is_moonraker_on_same_host() directly.
    bool is_moonraker_remote();

    /// Locality verdict for the CURRENT connection: Unknown until its CONNECTED
    /// edge calls set_moonraker_is_remote(), which sets Local or Remote. Unlike
    /// is_moonraker_remote(), which reads 0 (local) before any verdict and
    /// keeps the previous printer's value across a switch, this never reports
    /// a verdict the current connection has not made. The value is current as
    /// soon as the setter returns; the moonraker_locality subject follows
    /// through the UI queue. Any thread.
    helix::MoonrakerLocality moonraker_locality() const {
        return static_cast<helix::MoonrakerLocality>(moonraker_locality_.load());
    }

    /// Back to Unknown. Called on every non-CONNECTED connection edge and on a
    /// printer switch. Thread-safe; the subject write is deferred.
    void clear_moonraker_locality();

    /**
     * @brief Set Klipper firmware state (thread-safe, async)
     *
     * Updates klippy_state subject via queue_update to ensure thread safety.
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
     * @brief Seed Klipper firmware state, but never override a live value
     *
     * For startup-only sources that describe the printer as it was when the
     * request was issued — `printer.info`'s `state` field, whose response can
     * land seconds after the WebSocket has already reported a newer state.
     * No-ops once any live source (a webhooks frame carrying an eventtime, or a
     * notify_klippy_* message) has set the state.
     *
     * @param state KlippyState enum value
     * @param state_message The reason reported with it (printer.info's
     *        state_message), applied with the state; empty leaves the message alone
     */
    void set_klippy_state_if_unseeded(KlippyState state, const std::string& state_message = "");

    /**
     * @brief Update printer capability subjects from PrinterDiscovery
     *
     * Updates subjects that control visibility of pre-print option checkboxes.
     * Applies user-configured overrides from settings.json before updating subjects.
     * Called by main.cpp after MoonrakerClient::discover_printer() completes.
     *
     * @param hardware PrinterDiscovery populated from printer.objects.list
     */
    void set_hardware(helix::PrinterDiscovery hardware);

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
     * @brief Get cached hardware discovery result
     *
     * Provides access to the full list of heaters and sensors discovered
     * during hardware enumeration. Used by the chamber assignment UI to
     * populate dropdown options.
     *
     * @return Reference to the cached PrinterDiscovery instance
     */
    [[nodiscard]] const helix::PrinterDiscovery& get_discovery() const {
        return discovery_;
    }

    /**
     * @brief Check if Spoolman is available
     *
     * Reads the printer_has_spoolman subject value. Safe to call from any thread
     * (reads a single int).
     */
    bool is_spoolman_available() const {
        return lv_subject_get_int(capabilities_state_.subject(Capability::HasSpoolman)) == 1;
    }

    /**
     * @brief Check if Moonraker's job_queue component is present
     *
     * Reads the printer_has_job_queue subject value. Safe to call from any
     * thread (reads a single int).
     */
    bool is_job_queue_available() const {
        return lv_subject_get_int(capabilities_state_.subject(Capability::HasJobQueue)) == 1;
    }

    /// True if at least one enabled webcam has been detected
    bool has_webcam() const {
        return lv_subject_get_int(capabilities_state_.subject(Capability::HasWebcam)) == 1;
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
     * @brief Seed the default state of the synthesized timelapse pre-print option
     *
     * The moonraker-timelapse plugin has no per-print concept — the pre-print
     * toggle writes the GLOBAL `enabled` setting at print start. This seeds the
     * toggle's default from that global setting so a user who enabled timelapse
     * globally isn't silently disabled by starting a print without touching the
     * toggle (#1094). Fetched at discovery via machine.timelapse.get_settings.
     *
     * Thread-safe: Can be called from any thread, defers to the main thread and
     * re-synthesizes the option set there.
     *
     * @param enabled Global moonraker-timelapse `enabled` value
     */
    void set_timelapse_default_enabled(bool enabled);

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

    /// The plugin's link directory from its status (empty when not reported);
    /// any thread.
    void set_helix_plugin_link_dir(const std::string& dir);

    /// The version the plugin reports (empty when it did not answer); any thread.
    void set_helix_plugin_version(const std::string& version);

    /// Whether the plugin answered with `enabled: False`; any thread.
    void set_helix_plugin_disabled(bool disabled);

    /// The plugin's `features` map (empty when it reported none); any thread.
    void
    set_helix_plugin_features(helix::PluginFeatureMap features,
                              helix::FeatureAnnounce announce = helix::FeatureAnnounce::Always);

    /**
     * @brief Set how many PRINT OPTIONS rows the PRINT_START analysis adds
     *
     * Those rows come from macro operations with a skip parameter, on a
     * printer whose database entry declares no options; each one needs the
     * plugin to deliver its skip. Main thread only.
     */
    void set_macro_option_count(size_t count);

    /// A `_HELIX_PREP` skip was set and never consumed (main thread only).
    [[nodiscard]] bool skip_pending() const {
        return helix::skip_wrappers::any_skip_pending(skip_gates_);
    }

    /// Called on the main thread whenever a status frame changes the skip
    /// gates while skip_pending() holds. Empty clears it.
    void set_skip_pending_handler(std::function<void()> handler) {
        skip_pending_handler_ = std::move(handler);
    }

    /**
     * @brief Set printer kinematics type and update has_individual_xyz_homing and
     *        bed_moves subjects.
     *
     * Updates printer_has_individual_xyz_homing_ and printer_bed_moves_ subjects
     * based on kinematics type:
     *
     * - Deltas cannot home XYZ axes individually.
     * - CoreXY printers typically have bed moving on Z (Voron 2.4, RatRig).
     * - Cartesian/Delta printers typically have gantry moving on Z (Ender 3, Prusa).
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
     * @brief Resolve printer_is_enclosed and printer_can_bed_dry
     *        (prestonbrown/helixscreen#1730)
     *
     * Inputs: the printer database's enclosed flag, the enclosure override, a
     * configured chamber heater, a heated bed and the Z travel. Called wherever
     * one of those changes; writes only on change.
     */
    void refresh_bed_drying_capability();

    /**
     * @brief Whether a blocking non-print operation is currently in progress
     *
     * True while the printer is executing a blocking operation that holds
     * Klipper's single-threaded g-code lock but is NOT a normal file print:
     * homing (G28), BED_MESH_CALIBRATE, QUAD_GANTRY_LEVEL, PROBE_ACCURACY,
     * an interactive manual probe, or a long macro. Discretionary g-code sent
     * during such an op just queues until it finishes, then times out — so a
     * send-boundary guard uses this to refuse it early with a toast.
     *
     * Signal =
     *   (idle_timeout.state == "Printing" AND print_job_state NOT IN {PRINTING, PAUSED})
     *   OR manual_probe.is_active
     *
     * Real file prints (PRINTING/PAUSED) are excluded — mid-print fan/temp tweaks
     * are legitimate and Klipper handles them between moves.
     *
     * @return true if a blocking non-print operation is active
     */
    bool is_blocking_operation_active();

    /**
     * @brief Like is_blocking_operation_active(), but attributes self-inflicted busy
     *
     * Treats a busy episode the app's own jog started (see BusyEpisodeOwner) as
     * NOT blocking; motion sent into an episode someone else started is not.
     * Manual probe remains an absolute block. Guards for discretionary gcode use
     * THIS predicate so back-to-back jogs don't self-block — idle_timeout reports
     * "Printing" during any move, including our own jog (spec 2026-07-15).
     *
     * @return true if a blocking non-print operation NOT caused by the app is active
     */
    bool is_external_blocking_operation_active();

    /// App-initiated motion (jog) activity tracker; Task 3 stamps it from the
    /// motion API so is_external_blocking_operation_active() can subtract self-busy.
    helix::AppMotionActivity& app_motion_activity() {
        return app_motion_activity_;
    }

    /// App-initiated macro/homing/calibration/filament-op activity tracker.
    ///
    /// Consulted ONLY by the busy-queue toast decision in
    /// IMoonrakerAPI::execute_gcode, which suppresses the "printer is busy"
    /// notification when the blocking op is one the user just started here
    /// (prestonbrown/helixscreen#1206).
    ///
    /// Deliberately NOT read by is_blocking_operation_active() or
    /// is_external_blocking_operation_active(). Those predicates also gate
    /// motion, and subtracting app-initiated macros there would let a late jog
    /// through during a filament op — a toolhead-collision hazard (#1108).
    helix::AppMacroActivity& app_macro_activity() {
        return app_macro_activity_;
    }

    /// Every user command execute_gcode sends, discretionary fan/temp/LED
    /// included. Consulted ONLY by the busy-queue toast, through
    /// is_busy_episode_app_initiated(); the blocking-op predicates never read
    /// it, so a light tap cannot let a jog through during an external op.
    helix::BusyEpisodeOwner& app_command_episode() {
        return app_command_episode_;
    }

    /**
     * @brief Whether the current idle_timeout busy episode began with the user's own command
     *
     * Any gcode reads "Printing" while it runs, so a burst of light taps can
     * hold a settled busy episode up on an idle printer. Toast attribution only.
     */
    bool is_busy_episode_app_initiated();

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

    // ========================================================================
    // PRINTER TYPE
    // ========================================================================

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
     * @brief Record that an installed SET_GCODE_OFFSET wrapper owns z-offset
     *        persistence (zoffset:: matched a provider in discovery)
     *
     * Re-resolves the calibration strategy: with the offset persisted by the
     * wrapper, the probe fold in "Save Z Offset" would double-apply it on
     * every Klipper restart (prestonbrown/helixscreen#1401), so the strategy
     * becomes FIRMWARE_MANAGED and the save path stands down. Sticky across
     * printer-type re-resolution. Thread-safe: defers to the main thread.
     * @param provider_name for the log line only
     */
    void set_z_offset_external_persistence(const std::string& provider_name);

    /// The provider is gone or was never really there: restore the type-derived
    /// strategy. Two callers - rediscovery finding no provider (module
    /// uninstalled), and update_from_status() when a frame refutes a provider
    /// detected on an ambiguous signature (a SET_GCODE_OFFSET wrapper that
    /// stores nothing). Thread-safe like the setter.
    void clear_z_offset_external_persistence();

    /// Main-thread bodies; tests reach these directly.
    void set_z_offset_external_persistence_internal(const std::string& provider_name);
    void clear_z_offset_external_persistence_internal();

    // ========================================================================
    // MULTI-PRINTER SUBJECTS
    // ========================================================================

    /**
     * @brief Get the active printer display name subject
     *
     * String subject holding the human-readable name of the active printer.
     * Use with bind_text in XML to display the current printer name.
     */
    lv_subject_t* get_active_printer_name_subject() {
        return &active_printer_name_;
    }

    /**
     * @brief Set the active printer display name
     *
     * Updates the string subject with the given name. Main-thread only.
     *
     * @param name Human-readable printer name
     */
    void set_active_printer_name(const std::string& name);

  private:
    void unregister_static_deinit();

    /// RAII manager for automatic subject cleanup - deinits all subjects on destruction
    SubjectManager subjects_;

    /// Temperature state component (extruder and bed temperatures)
    helix::PrinterTemperatureState temperature_state_;

    /// Motion state component (position, speed/flow, z-offset)
    helix::PrinterMotionState motion_state_;

    /// Fan state component (fan speed, multi-fan tracking)
    helix::PrinterFanState fan_state_;

    /// Print state component (progress, state, timing, layers, print start)
    helix::PrinterPrintState print_domain_;

    /// Capabilities state component (hardware capabilities, feature availability)
    helix::PrinterCapabilitiesState capabilities_state_;

    /// Plugin status component (helix_plugin_installed, helix_macros_status)
    helix::PrinterPluginStatusState plugin_status_state_;

    /// Calibration state component (firmware retraction, manual probe, motor state)
    helix::PrinterCalibrationState calibration_state_;

    /// App-initiated motion (jog) activity tracker for busy-guard attribution
    helix::AppMotionActivity app_motion_activity_;

    /// App-initiated macro/filament-op activity tracker for busy-TOAST attribution
    /// only — never consulted by the blocking-op predicates (see accessor).
    helix::AppMacroActivity app_macro_activity_;

    /// User-command sends for busy-TOAST attribution only (see accessor).
    helix::BusyEpisodeOwner app_command_episode_;

    /// Hardware validation state component (issue counts, severity, status text)
    helix::PrinterHardwareValidationState hardware_validation_state_;

    /// Composite visibility state component (has_any_preprint_options aggregate)
    helix::PrinterCompositeVisibilityState composite_visibility_state_;

    /// Network state component (connection, klippy, nav buttons)
    helix::PrinterNetworkState network_state_;
    std::atomic<int> moonraker_locality_{static_cast<int>(helix::MoonrakerLocality::Unknown)};

    /// Versions state component (klipper and moonraker version strings)
    helix::PrinterVersionsState versions_state_;

    /// Excluded objects state component (excluded_objects_version, excluded_objects set)
    helix::PrinterExcludedObjectsState excluded_objects_state_;

    /// Printer type, its pre-print option set and z-offset calibration strategy
    helix::PrinterProfileState profile_state_;

    // Multi-printer subjects (owned directly by PrinterState)
    lv_subject_t active_printer_name_{};
    char active_printer_name_buf_[128];

    // Initialization guard to prevent multiple subject initializations
    bool subjects_initialized_ = false;

    /// Generation guard for the setters that defer their work to the main
    /// thread. Invalidated by `deinit_subjects()` and by destruction, so a
    /// callback still queued when the subjects go away is dropped instead of
    /// running against a torn-down subject tree (#1165, #1146). Distinct from
    /// the `SubjectLifetime` tokens handed to observers, which are
    /// `shared_ptr<bool>` death signals and carry no deferral machinery.
    AsyncLifetimeGuard async_lifetime_;

    /// Bumped by set_hardware(), so firmware option settings read off the
    /// previous machine's status cannot land on the next one.
    std::atomic<uint64_t> hardware_epoch_{0};

    // Cached display pointer to detect LVGL reinitialization (for test isolation)
    lv_display_t* cached_display_ = nullptr;

    // Capability override layer (user config overrides for auto-detected capabilities)
    CapabilityOverrides capability_overrides_;

    // Cached hardware discovery result (for UI access to heater/sensor lists)
    helix::PrinterDiscovery discovery_;

    /// Last kinematics string (to skip redundant recomputation)
    std::string last_kinematics_;

    /// Auto-detected bed_moves value from kinematics (before user override)
    bool auto_detected_bed_moves_ = false;
    size_t macro_option_count_ = 0; ///< See set_macro_option_count()

    /// Klipper pause_resume.is_paused: true when the print is paused via PAUSE gcode
    bool is_paused_ = false;

    // ============================================================================
    // Main-thread internal methods (run from queued callbacks)
    // ============================================================================
    // These methods contain the actual LVGL subject updates and must only be called
    // from the main thread. Public setters such as set_klipper_version() reach them
    // through async_lifetime_.defer(), ensuring thread safety.

    friend class PrinterStateTestAccess;

    void set_klipper_version_internal(const std::string& version);
    void set_moonraker_version_internal(const std::string& version);
    void set_os_version_internal(const std::string& version);
    void set_klippy_state_internal(KlippyState state);
    void set_printer_type_internal(const std::string& type);

    /// Merge the settings a self-storing firmware currently holds into the
    /// pre-print option defaults, keyed by option id, and resynthesise if any
    /// changed. Safe from any thread. Merges rather than replaces: Moonraker
    /// sends deltas, so a frame mentioning one setting is silent about the
    /// rest, not a report that they are off.
    void merge_firmware_option_defaults(std::map<std::string, bool> defaults);

    /// Main-thread half of set_klippy_state_if_unseeded(): re-checks the guard in
    /// the same serialized order as the webhooks parse, then applies.
    void set_klippy_state_if_unseeded_internal(KlippyState state, const std::string& state_message);

    /**
     * @brief Synthesize runtime-dependent options (timelapse, etc.) into the
     *        cached `PrePrintOptionSet`.
     *
     * Some options aren't declared in the printer database — they're driven
     * by runtime capability discovery (e.g. the `timelapse` toggle only
     * appears when the moonraker-timelapse plugin is installed). This helper
     * appends those options to whatever the database loaded, so the rest of
     * the system can treat them uniformly.
     *
     * Idempotent — call after the database load and again whenever one of
     * the runtime capabilities changes (e.g. `set_timelapse_available()`).
     * Re-running clears any previously synthesized options before re-adding
     * the ones that should currently be present.
     */
    void apply_dynamic_options();

    /// The moonraker-timelapse plugin is available (main thread only).
    bool timelapse_available();

    /// What offers the leveling skip toggles, folded from status frames.
    helix::skip_wrappers::Gates skip_gates_;
    std::function<void()> skip_pending_handler_;

    /**
     * @brief Update combined nav_buttons_enabled subject
     *
     * Recalculates nav_buttons_enabled based on connection and klippy state.
     * Called whenever printer_connection_state or klippy_state changes.
     */
    void update_nav_buttons_enabled();

    /**
     * @brief Refresh the has_any_preprint_options aggregate
     *
     * Recomputes the aggregate visibility subject from current plugin status,
     * capability subjects, and the framework option count. Called whenever
     * helix_plugin_installed, printer_has_*, or the cached PrePrintOptionSet
     * change. Must be called from the main thread (typically via async callbacks).
     */
    void update_gcode_modification_visibility();
};

} // namespace helix
