// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file moonraker_advanced_api.h
 * @brief Advanced panel operations via Moonraker
 *
 * Extracted from MoonrakerAPI to encapsulate bed mesh, input shaper, PID calibration,
 * machine limits, and macro execution in a dedicated class. Uses MoonrakerClient for
 * JSON-RPC transport and MoonrakerAPI for G-code execution.
 */

#pragma once

#include "advanced_panel_types.h"
#include "belt_tension_types.h"
#include "calibration_types.h"
#include "moonraker_error.h"
#include "moonraker_types.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

// Forward declarations
namespace helix {
class MoonrakerClient;
} // namespace helix
class MoonrakerAPI;

/**
 * @brief Advanced Panel Operations API via Moonraker
 *
 * Provides high-level operations for bed mesh management, input shaper calibration,
 * PID tuning, machine limits configuration, and macro execution. These operations
 * typically involve long-running G-code commands with response parsing via
 * notify_gcode_response callbacks.
 *
 * All async methods use callbacks. Long-running operations use Collector state machines
 * that monitor gcode_response notifications for progress and results.
 *
 * Usage:
 *   MoonrakerAdvancedAPI advanced(client, api);
 *   advanced.start_bed_mesh_calibrate(on_progress, on_complete, on_error);
 */
class MoonrakerAdvancedAPI {
  public:
    // ========== Timeout constants for long-running G-code commands ==========
    static constexpr uint32_t CALIBRATION_TIMEOUT_MS =
        300000; // 5 min - BED_MESH_CALIBRATE, SCREWS_TILT_CALCULATE
    static constexpr uint32_t LEVELING_TIMEOUT_MS = 600000; // 10 min - QGL, Z_TILT_ADJUST
    static constexpr uint32_t SHAPER_TIMEOUT_MS =
        300000; // 5 min - SHAPER_CALIBRATE, MEASURE_AXES_NOISE
    static constexpr uint32_t PID_TIMEOUT_MS =
        1200000; // 20 min - PID_CALIBRATE (slow-cooling beds, e.g. AD5M Pro, exceed 15 min)
    static constexpr uint32_t MPC_TIMEOUT_MS = 1200000; // 20 min - MPC_CALIBRATE
    static constexpr uint32_t PROBING_TIMEOUT_MS =
        180000; // 3 min - PROBE_CALIBRATE, Z_ENDSTOP_CALIBRATE
    static constexpr uint32_t BELT_TENSION_TIMEOUT_MS = 120000; // 2 min per path

    using SuccessCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const MoonrakerError&)>;

    /// Progress callback for bed mesh calibration: (current_probe, total_probes)
    using BedMeshProgressCallback = std::function<void(int current, int total)>;

    /// Callback for accelerometer noise level check (noise value 0-1000+, <100 is good)
    using NoiseCheckCallback = std::function<void(float noise_level)>;

    /// Callback for input shaper configuration query
    using InputShaperConfigCallback = std::function<void(const InputShaperConfig&)>;

    /// Callback for heater control type query (returns "pid", "mpc", etc.)
    using HeaterControlTypeCallback = std::function<void(const std::string& control_type)>;

    /// Callback for PID calibration progress (sample number, tolerance value; -1.0 = n/a)
    using PIDProgressCallback = std::function<void(int sample, float tolerance)>;

    /// Callback for PID calibration result
    using PIDCalibrateCallback = std::function<void(float kp, float ki, float kd)>;

    /// Result struct for MPC calibration
    struct MPCResult {
        float block_heat_capacity = 0;
        float sensor_responsiveness = 0;
        float ambient_transfer = 0;
        std::string fan_ambient_transfer; // Comma-separated values like "0.12, 0.18, 0.25"
    };

    /// Callback for MPC calibration result
    using MPCCalibrateCallback = std::function<void(const MPCResult&)>;

    /// Progress callback for MPC calibration (phase, total_phases, description)
    using MPCProgressCallback =
        std::function<void(int phase, int total_phases, const std::string& description)>;

    /**
     * @brief Constructor
     *
     * @param client MoonrakerClient instance (must remain valid during API lifetime)
     * @param api MoonrakerAPI instance for G-code execution (must remain valid during API lifetime)
     */
    MoonrakerAdvancedAPI(helix::MoonrakerClient& client, MoonrakerAPI& api);
    virtual ~MoonrakerAdvancedAPI() = default;

    // ========================================================================
    // Bed Mesh Operations (with internal state)
    // ========================================================================

    /**
     * @brief Get currently active bed mesh profile
     *
     * Returns pointer to the active mesh profile loaded from Moonraker's
     * bed_mesh object. The probed_matrix field contains the 2D Z-height
     * array ready for rendering.
     *
     * @return Pointer to active mesh profile, or nullptr if none loaded
     */
    const BedMeshProfile* get_active_bed_mesh() const;

    /**
     * @brief Update bed mesh data from Moonraker status
     *
     * Called by MoonrakerClient when bed_mesh data is received from
     * Moonraker subscriptions. Parses the JSON and updates local storage.
     *
     * Thread-safe: Uses internal mutex for synchronization.
     *
     * @param bed_mesh_data JSON object containing bed_mesh status fields
     */
    void update_bed_mesh(const json& bed_mesh_data);

    /**
     * @brief Get list of available mesh profile names
     *
     * Returns profile names from bed_mesh.profiles (e.g., "default",
     * "adaptive", "calibration"). Empty vector if no profiles available
     * or discovery hasn't completed.
     *
     * @return Vector of profile names
     */
    std::vector<std::string> get_bed_mesh_profiles() const;

    /**
     * @brief Check if bed mesh data is available
     *
     * @return true if a mesh profile with valid probed_matrix is loaded
     */
    bool has_bed_mesh() const;

    /**
     * @brief Get mesh data for a specific stored profile
     *
     * Returns the mesh data for any stored profile (not just the active one).
     * This enables showing Z range for all profiles in the list.
     *
     * @param profile_name Name of the profile to retrieve
     * @return Pointer to profile data, or nullptr if not found
     */
    const BedMeshProfile* get_bed_mesh_profile(const std::string& profile_name) const;

    /**
     * @brief Get set of currently excluded object names (async)
     *
     * Queries Klipper's exclude_object module for the list of objects
     * that have been excluded from the current print.
     *
     * @param on_success Callback with set of excluded object names
     * @param on_error Error callback
     */
    void get_excluded_objects(std::function<void(const std::set<std::string>&)> on_success,
                              ErrorCallback on_error);

    /**
     * @brief Get list of available objects in current print (async)
     *
     * Queries Klipper's exclude_object module for the list of objects
     * defined in the current G-code file (from EXCLUDE_OBJECT_DEFINE).
     *
     * @param on_success Callback with vector of available object names
     * @param on_error Error callback
     */
    void get_available_objects(std::function<void(const std::vector<std::string>&)> on_success,
                               ErrorCallback on_error);

    // ========================================================================
    // Bed Leveling Operations
    // ========================================================================

    /**
     * @brief Start automatic bed mesh calibration with progress tracking
     *
     * Executes BED_MESH_CALIBRATE command and tracks probe progress via
     * notify_gcode_response parsing.
     *
     * @param on_progress Called for each probe point (current, total)
     * @param on_complete Called when calibration completes successfully
     * @param on_error Called on failure
     * @param expected_probes Hint for total probe count (from configfile).
     *        Used as denominator when firmware doesn't emit "Probing point X/Y"
     *        progress lines.  0 = unknown (shows indeterminate spinner).
     * @param probe_samples Number of probe samples per mesh point (from [probe]
     *        or [bltouch] config).  Used to divide the fallback "probe at" line
     *        count back to mesh points.  Default 1.
     */
    virtual void start_bed_mesh_calibrate(BedMeshProgressCallback on_progress,
                                          SuccessCallback on_complete, ErrorCallback on_error,
                                          int expected_probes = 0, int probe_samples = 1);

    /**
     * @brief Calculate screw adjustments for manual bed leveling
     *
     * Executes SCREWS_TILT_CALCULATE command. Requires [screws_tilt_adjust]
     * section in printer.cfg.
     *
     * @param on_success Called with screw adjustment results
     * @param on_error Called on failure
     */
    virtual void calculate_screws_tilt(helix::ScrewTiltCallback on_success, ErrorCallback on_error);

    /**
     * @brief Run Quad Gantry Level
     *
     * Executes QUAD_GANTRY_LEVEL command for Voron-style printers.
     *
     * @param on_success Called when leveling completes
     * @param on_error Called on failure
     */
    virtual void run_qgl(SuccessCallback on_success, ErrorCallback on_error);

    /**
     * @brief Run Z-Tilt Adjust
     *
     * Executes Z_TILT_ADJUST command for multi-motor Z printers.
     *
     * @param on_success Called when adjustment completes
     * @param on_error Called on failure
     */
    virtual void run_z_tilt_adjust(SuccessCallback on_success, ErrorCallback on_error);

    // ========================================================================
    // Input Shaper Operations
    // ========================================================================

    /**
     * @brief Start resonance test for input shaper calibration
     *
     * Executes TEST_RESONANCES command for the specified axis.
     * Requires accelerometer configuration in printer.cfg.
     *
     * @param axis Axis to test ('X' or 'Y')
     * @param on_progress Called with progress percentage (0-100)
     * @param on_complete Called with test results
     * @param on_error Called on failure
     */
    virtual void start_resonance_test(char axis, helix::AdvancedProgressCallback on_progress,
                                      helix::InputShaperCallback on_complete,
                                      ErrorCallback on_error);

    /**
     * @brief Start Klippain Shake&Tune calibration
     *
     * Executes AXES_SHAPER_CALIBRATION macro from Klippain.
     * Provides enhanced calibration with graphs.
     *
     * @param axis Axis to calibrate ("X", "Y", or "all")
     * @param on_success Called when calibration completes
     * @param on_error Called on failure
     */
    virtual void start_klippain_shaper_calibration(const std::string& axis,
                                                   SuccessCallback on_success,
                                                   ErrorCallback on_error);

    /**
     * @brief Apply input shaper settings
     *
     * Sets the shaper type and frequency via SET_INPUT_SHAPER command.
     *
     * @param axis Axis to configure ('X' or 'Y')
     * @param shaper_type Shaper algorithm (e.g., "mzv", "ei")
     * @param freq_hz Shaper frequency in Hz
     * @param on_success Called when settings are applied
     * @param on_error Called on failure
     */
    virtual void set_input_shaper(char axis, const std::string& shaper_type, double freq_hz,
                                  SuccessCallback on_success, ErrorCallback on_error);

    /**
     * @brief Check accelerometer noise level
     *
     * Runs MEASURE_AXES_NOISE G-code command to measure the ambient noise
     * level of the accelerometer. Used to verify ADXL345 is working correctly
     * before running resonance tests.
     *
     * @param on_complete Called with noise level on success
     * @param on_error Called on failure (e.g., no accelerometer configured)
     */
    virtual void measure_axes_noise(NoiseCheckCallback on_complete, ErrorCallback on_error);

    /**
     * @brief Get current input shaper configuration
     *
     * Queries the printer state to retrieve the currently active input
     * shaper settings for both X and Y axes.
     *
     * @param on_success Called with current InputShaperConfig
     * @param on_error Called on failure
     */
    virtual void get_input_shaper_config(InputShaperConfigCallback on_success,
                                         ErrorCallback on_error);

    // ========================================================================
    // PID Calibration Operations
    // ========================================================================

    /**
     * @brief Fetch current PID values for a heater from printer configuration
     *
     * Queries configfile.settings to get the currently active PID parameters.
     * Used to show old->new deltas after PID calibration.
     *
     * @param heater Heater name ("extruder" or "heater_bed")
     * @param on_complete Called with current Kp, Ki, Kd values
     * @param on_error Called if values cannot be retrieved
     */
    virtual void get_heater_pid_values(const std::string& heater, PIDCalibrateCallback on_complete,
                                       ErrorCallback on_error);

    /**
     * @brief Query the control type for a heater from printer configuration
     *
     * Queries configfile.settings to determine if a heater uses PID or MPC control.
     * Returns "pid" by default if the control key is missing.
     *
     * @param heater Heater name ("extruder", "heater_bed", etc.)
     * @param on_complete Called with control type string ("pid", "mpc", etc.)
     * @param on_error Called if the heater cannot be found in config
     */
    virtual void get_heater_control_type(const std::string& heater,
                                         HeaterControlTypeCallback on_complete,
                                         ErrorCallback on_error);

    /**
     * @brief Start PID calibration for a heater
     *
     * Executes PID_CALIBRATE HEATER={heater} TARGET={target_temp} command
     * and collects results via gcode_response parsing.
     *
     * @param heater Heater name ("extruder" or "heater_bed")
     * @param target_temp Target temperature for calibration
     * @param on_complete Called with PID values on success
     * @param on_error Called on failure
     */
    virtual void start_pid_calibrate(const std::string& heater, int target_temp,
                                     PIDCalibrateCallback on_complete, ErrorCallback on_error,
                                     PIDProgressCallback on_progress = nullptr);

    /**
     * @brief Start MPC calibration for a heater (Kalico/Danger Klipper)
     *
     * Executes MPC_CALIBRATE HEATER={heater} TARGET={target_temp} command
     * and collects multi-line results via gcode_response parsing.
     *
     * MPC calibration goes through multiple phases: ambient settling, heatup test,
     * and fan breakpoint measurements before producing final calibration values.
     *
     * @param heater Heater name ("extruder", etc.)
     * @param target_temp Target temperature for calibration
     * @param fan_breakpoints Number of fan speed breakpoints to measure (0 = default)
     * @param on_complete Called with MPCResult on success
     * @param on_error Called on failure
     * @param on_progress Called for each calibration phase (phase, total_phases, description)
     */
    virtual void start_mpc_calibrate(const std::string& heater, int target_temp,
                                     int fan_breakpoints, MPCCalibrateCallback on_complete,
                                     ErrorCallback on_error,
                                     MPCProgressCallback on_progress = nullptr);

    // ========================================================================
    // Machine Limits Operations
    // ========================================================================

    /**
     * @brief Get current machine limits
     *
     * Queries toolhead object for velocity and acceleration limits.
     *
     * @param on_success Called with current limits
     * @param on_error Called on failure
     */
    virtual void get_machine_limits(helix::MachineLimitsCallback on_success,
                                    ErrorCallback on_error);

    /**
     * @brief Set machine limits (temporary, not saved to config)
     *
     * Uses SET_VELOCITY_LIMIT command. Changes are lost on Klipper restart.
     *
     * @param limits New limits to apply
     * @param on_success Called when limits are applied
     * @param on_error Called on failure
     */
    virtual void set_machine_limits(const MachineLimits& limits, SuccessCallback on_success,
                                    ErrorCallback on_error);

    /**
     * @brief Save current configuration to printer.cfg
     *
     * Executes SAVE_CONFIG command. This will restart Klipper.
     *
     * @param on_success Called when save is initiated
     * @param on_error Called on failure
     */
    virtual void save_config(SuccessCallback on_success, ErrorCallback on_error);

    // ========================================================================
    // Macro Operations
    // ========================================================================

    /**
     * @brief Execute a G-code macro with optional parameters
     *
     * @param name Macro name (e.g., "CLEAN_NOZZLE")
     * @param params Parameter map (e.g., {"TEMP": "210"})
     * @param on_success Called when macro execution starts
     * @param on_error Called on failure
     */
    virtual void execute_macro(const std::string& name,
                               const std::map<std::string, std::string>& params,
                               SuccessCallback on_success, ErrorCallback on_error,
                               uint32_t timeout_ms = 0, bool suppress_auto_toast = false);

    /**
     * @brief Get list of user-visible macros
     *
     * Returns macros filtered by category, excluding system macros
     * (those starting with _) unless explicitly requested.
     *
     * @param include_system Include _* system macros
     * @return Vector of macro information
     */
    std::vector<MacroInfo> get_user_macros(bool include_system = false) const;

    // ========================================================================
    // Belt Tension Operations
    // ========================================================================

    /// Callback for belt resonance test completion (returns output name for CSV lookup)
    using BeltResonanceCallback = std::function<void(const std::string& csv_path)>;

    /// Callback for belt hardware detection
    using BeltHardwareCallback =
        std::function<void(const helix::calibration::BeltTensionHardware&)>;

    /**
     * @brief Detect printer hardware for belt tension calibration
     *
     * Two-phase detection: queries printer.objects.list for ADXL/PWM presence,
     * then printer.objects.query for kinematics type.
     *
     * @param on_complete Called with detected hardware capabilities
     * @param on_error Called on failure
     */
    virtual void detect_belt_hardware(BeltHardwareCallback on_complete, ErrorCallback on_error);

    /**
     * @brief Run TEST_RESONANCES for belt tension measurement
     *
     * Executes TEST_RESONANCES with OUTPUT=raw_data to produce a CSV file
     * of accelerometer data for belt tension analysis.
     *
     * @param axis_param Axis parameter: "1,1" for CoreXY Path A, "1,-1" for Path B, "X"/"Y" for
     * Cartesian
     * @param output_name Name for the CSV output file
     * @param on_progress Called with progress percentage (0-100)
     * @param on_complete Called with output name on success
     * @param on_error Called on failure
     */
    virtual void test_belt_resonance(const std::string& axis_param, const std::string& output_name,
                                     helix::AdvancedProgressCallback on_progress,
                                     BeltResonanceCallback on_complete, ErrorCallback on_error);

    /**
     * @brief Run TEST_RESONANCES at a fixed frequency (for strobe mode)
     *
     * Holds near freq_hz for ~5 seconds by using a narrow frequency band
     * (FREQ_START=F FREQ_END=F+0.5 HZ_PER_SEC=0.1).
     *
     * @param axis_param Axis parameter (same as test_belt_resonance)
     * @param freq_hz Frequency to excite at
     * @param on_complete Called when excitation completes
     * @param on_error Called on failure
     */
    virtual void excite_belt_at_frequency(const std::string& axis_param, float freq_hz,
                                          SuccessCallback on_complete, ErrorCallback on_error);

    /**
     * @brief Set PWM LED strobe frequency
     *
     * Controls a Klipper [pwm_cycle_time] pin for visual strobe tuning.
     * Pass freq_hz <= 0 to turn off the strobe.
     *
     * @param pin_name Klipper pin name (from [pwm_cycle_time] section)
     * @param freq_hz Strobe frequency in Hz, 0 to turn off
     * @param on_success Called on success
     * @param on_error Called on failure
     */
    virtual void set_strobe_frequency(const std::string& pin_name, float freq_hz,
                                      SuccessCallback on_success, ErrorCallback on_error);

    /**
     * @brief Download raw accelerometer CSV from Klipper data store
     *
     * Retrieves the raw resonance CSV file produced by TEST_RESONANCES.
     * The file is typically at /tmp/raw_data_<name>*.csv on the printer host.
     *
     * @param filename CSV filename to download
     * @param on_complete Called with raw CSV data on success
     * @param on_error Called on failure
     */
    virtual void download_accel_csv(const std::string& filename,
                                    std::function<void(const std::string& csv_data)> on_complete,
                                    ErrorCallback on_error);

  protected:
    helix::MoonrakerClient& client_;
    MoonrakerAPI& api_;

  private:
    // Bed mesh storage
    BedMeshProfile active_bed_mesh_;
    std::vector<std::string> bed_mesh_profiles_;
    std::map<std::string, BedMeshProfile> stored_bed_mesh_profiles_;
    mutable std::mutex bed_mesh_mutex_;
};
