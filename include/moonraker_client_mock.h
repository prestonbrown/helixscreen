// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "moonraker_client.h"
#include "moonraker_types.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "hv/json.hpp"

// Forward declaration for shared state
class MockPrinterState;

// Forward declaration for MoonrakerClientMock (needed for internal handler registry)
class MoonrakerClientMock;

// Forward declaration for the test-access friend (tests/test_helpers/)
namespace helix {
class MoonrakerClientMockTestAccess;
} // namespace helix

// Forward declaration for internal handler registry
namespace mock_internal {
using MethodHandler =
    std::function<bool(MoonrakerClientMock*, const json&, std::function<void(const json&)>,
                       std::function<void(const MoonrakerError&)>)>;
} // namespace mock_internal

/**
 * @brief Mock Moonraker client for testing without real printer connection
 *
 * Simulates printer hardware discovery with configurable test data.
 * Useful for UI development and testing without physical hardware.
 *
 * Inherits from MoonrakerClient to provide drop-in replacement compatibility.
 * Overrides discover_printer() to populate test data without WebSocket connection.
 */
class MoonrakerClientMock : public helix::MoonrakerClient {
  public:
    enum class PrinterType {
        VORON_24,           // Voron 2.4 (CoreXY, chamber heating)
        VORON_TRIDENT,      // Voron Trident (3Z, CoreXY)
        CREALITY_K1,        // Creality K1/K1 Max (bed slinger style)
        FLASHFORGE_AD5M,    // FlashForge Adventurer 5M (enclosed)
        GENERIC_COREXY,     // Generic CoreXY printer
        GENERIC_BEDSLINGER, // Generic i3-style printer
        MULTI_EXTRUDER      // Multi-extruder test case (2 extruders)
    };

    /**
     * @brief Print simulation phase state machine
     *
     * Tracks the current phase of a simulated print job, including
     * thermal preheating and cooldown after completion.
     */
    enum class MockPrintPhase {
        IDLE,      ///< No print, room temperature
        PREHEAT,   ///< Heating to target temps before print starts
        PRINTING,  ///< Active printing, progress advancing
        PAUSED,    ///< Print paused, temps maintained
        COMPLETE,  ///< Print finished, cooling down
        CANCELLED, ///< Print cancelled, cooling down
        ERROR      ///< Emergency stop or failure
    };

    /**
     * @brief Klipper service state (matches Moonraker webhooks.state)
     *
     * Tracks the state of the Klipper firmware service, used during
     * RESTART/FIRMWARE_RESTART simulation.
     */
    enum class KlippyState {
        READY,    ///< Normal operation, printer ready
        STARTUP,  ///< Restarting (during RESTART/FIRMWARE_RESTART)
        SHUTDOWN, ///< Emergency shutdown (M112)
        ERROR     ///< Klipper error state
    };

    /**
     * @brief Metadata extracted from G-code for print simulation
     *
     * Stores print parameters extracted from G-code file metadata
     * to drive realistic simulation timing and thermal behavior.
     */
    struct MockPrintMetadata {
        double estimated_time_seconds = 300.0;  ///< Default 5 min if not in file
        uint32_t layer_count = 100;             ///< Default 100 layers
        double target_bed_temp = 60.0;          ///< First layer bed temp
        double target_nozzle_temp = 210.0;      ///< First layer nozzle temp
        double filament_mm = 0.0;               ///< Total filament length
        std::vector<double> filament_weights_g; ///< Per-tool grams (empty when slicer didn't emit)

        void reset() {
            estimated_time_seconds = 300.0;
            layer_count = 100;
            target_bed_temp = 60.0;
            target_nozzle_temp = 210.0;
            filament_mm = 0.0;
            filament_weights_g.clear();
        }
    };

    /**
     * @brief Construct mock client with default real-time simulation speed
     * @param type Printer type to simulate
     */
    MoonrakerClientMock(PrinterType type = PrinterType::VORON_24);

    /**
     * @brief Construct mock client with custom simulation speedup
     * @param type Printer type to simulate
     * @param speedup_factor Simulation speed multiplier (e.g., 10.0 = 10x faster)
     */
    MoonrakerClientMock(PrinterType type, double speedup_factor);

    ~MoonrakerClientMock();

    // Prevent copying (has thread state)
    MoonrakerClientMock(const MoonrakerClientMock&) = delete;
    MoonrakerClientMock& operator=(const MoonrakerClientMock&) = delete;

    /**
     * @brief Set simulation speedup factor at runtime
     *
     * Affects both thermal simulation and print progress rates.
     * A factor of 10.0 means a 30-minute print completes in 3 minutes wall-clock.
     *
     * @param factor Speed multiplier (clamped to [0.1, 10000])
     */
    void set_simulation_speedup(double factor);

    /**
     * @brief Get current simulation speedup factor
     * @return Current speedup multiplier (1.0 = real-time)
     */
    double get_simulation_speedup() const;

    /**
     * @brief Arm a capture replay through the real dispatch paths
     *
     * Parses a replay script (see tests/fixtures/k1c_flowrate_replay.json for
     * the format and the extractor that produced it) and, once connect()
     * completes its initial state, walks its timed events through
     * dispatch_method_callback("notify_gcode_response") and
     * dispatch_status_update() — the same entry points the live WebSocket
     * uses. Bed-mesh payloads traverse parse_incoming_bed_mesh and the real
     * MoonrakerAPI callback chain, so manager wiring and observers are
     * exercised exactly as on a printer. Event times are divided by the
     * simulation speedup.
     *
     * @param json_path Path to the replay script; parsed immediately so a
     *                  malformed script fails fast. Returns false and logs
     *                  if it cannot be read or parsed.
     */
    bool arm_event_replay(const std::string& json_path);

    /**
     * @brief Get current print simulation phase
     * @return Current phase of the print simulation state machine
     */
    MockPrintPhase get_print_phase() const {
        return print_phase_.load();
    }

    /**
     * @brief Get current Klipper service state
     * @return Current klippy_state (READY, STARTUP, SHUTDOWN, ERROR)
     */
    KlippyState get_klippy_state() const {
        return klippy_state_.load();
    }

    /**
     * @brief Set Klipper service state (for testing)
     * @param state New state to set
     */
    void set_klippy_state(KlippyState state) {
        klippy_state_.store(state);
    }

    /**
     * @brief Check if motors are currently enabled
     * @return true if motors enabled (Ready/Printing), false if disabled (Idle via M84)
     */
    bool are_motors_enabled() const {
        return motors_enabled_.load();
    }

    /**
     * @brief Set idle timeout duration in seconds
     * @param seconds Timeout duration (default 600 = 10 minutes)
     */
    void set_idle_timeout_seconds(uint32_t seconds) {
        idle_timeout_seconds_.store(seconds);
    }

    /**
     * @brief Get idle timeout duration in seconds
     * @return Current timeout setting
     */
    uint32_t get_idle_timeout_seconds() const {
        return idle_timeout_seconds_.load();
    }

    /**
     * @brief Check if idle timeout has been triggered
     * @return true if idle timeout triggered (motors disabled due to inactivity)
     */
    bool is_idle_timeout_triggered() const {
        return idle_timeout_triggered_.load();
    }

    /**
     * @brief Reset idle timeout timer
     *
     * Called when activity occurs (homing, movement, temperature commands).
     * Resets the inactivity timer and clears the triggered flag.
     */
    void reset_idle_timeout();

    /**
     * @brief Get current layer number in simulated print
     * @return Current layer (0-based), or 0 if not printing
     */
    int get_current_layer() const;

    /**
     * @brief Get total layer count for current print
     * @return Total layers from G-code metadata, or 0 if not printing
     */
    int get_total_layers() const;

    /**
     * @brief Get set of excluded object names
     *
     * Returns the names of objects that have been excluded via
     * EXCLUDE_OBJECT G-code commands during the current print.
     *
     * If a shared MockPrinterState is set, returns from shared state.
     * Otherwise falls back to local excluded_objects_ member.
     *
     * @return Set of excluded object names (thread-safe copy)
     */
    std::set<std::string> get_excluded_objects() const;

    /**
     * @brief Set shared mock state for coordination with MoonrakerAPIMock
     *
     * When set, excluded objects and other state changes are propagated
     * to the shared state, allowing MoonrakerAPIMock to return consistent
     * values when queried.
     *
     * @param state Shared state pointer (can be nullptr to disable)
     */
    void set_mock_state(std::shared_ptr<MockPrinterState> state);

    /**
     * @brief Get shared mock state (may be nullptr)
     *
     * @return Shared state pointer, or nullptr if not set
     */
    std::shared_ptr<MockPrinterState> get_mock_state() const {
        return mock_state_;
    }

    /**
     * @brief Simulate WebSocket connection (no real network I/O)
     *
     * Overrides base class to simulate successful connection without
     * actual WebSocket establishment. Immediately invokes on_connected callback.
     *
     * @param url WebSocket URL (ignored in mock)
     * @param on_connected Callback invoked immediately
     * @param on_disconnected Callback stored but never invoked in mock
     * @return Always returns 0 (success)
     */
    int connect(const char* url, std::function<void()> on_connected,
                std::function<void()> on_disconnected) override;

    /**
     * @brief Simulate printer hardware discovery
     *
     * Overrides base class method to immediately populate hardware lists
     * based on configured printer type and invoke completion callback.
     * If Klippy state is not READY, invokes error callback instead.
     *
     * @param on_complete Callback invoked after discovery completes
     * @param on_error Optional callback invoked if discovery fails
     */
    void
    discover_printer(std::function<void()> on_complete,
                     std::function<void(const std::string& reason)> on_error = nullptr) override;

    /**
     * @brief Simulate WebSocket disconnection (no real network I/O)
     *
     * Overrides base class to simulate disconnection without actual WebSocket teardown.
     */
    void disconnect() override;

    /**
     * @brief Simulate JSON-RPC request without parameters
     *
     * Overrides base class to log and return success without network I/O.
     *
     * @param method RPC method name
     * @return Always returns 0 (success)
     */
    int send_jsonrpc(const std::string& method) override;

    /**
     * @brief Simulate JSON-RPC request with parameters
     *
     * Overrides base class to log and return success without network I/O.
     *
     * @param method RPC method name
     * @param params JSON parameters object (ignored in mock)
     * @return Always returns 0 (success)
     */
    int send_jsonrpc(const std::string& method, const json& params) override;

    /**
     * @brief Simulate JSON-RPC request with callback
     *
     * Overrides base class to log and return success without network I/O.
     *
     * @param method RPC method name
     * @param params JSON parameters object (ignored in mock)
     * @param cb Callback function (not invoked in mock)
     * @return Always returns 0 (success)
     */
    helix::RequestId send_jsonrpc(const std::string& method, const json& params,
                                  std::function<void(const json&)> cb) override;

    /**
     * @brief Simulate JSON-RPC request with success/error callbacks
     *
     * Overrides base class to log and return success without network I/O.
     *
     * @param method RPC method name
     * @param params JSON parameters object (ignored in mock)
     * @param success_cb Success callback (not invoked in mock)
     * @param error_cb Error callback (not invoked in mock)
     * @param timeout_ms Timeout (ignored in mock)
     * @param silent Silent mode (ignored in mock)
     * @param intent Explicit error-reporting intent (include/rpc_error_policy.h).
     *        Omitted, it is inferred from @p silent and the presence of
     *        @p error_cb, exactly as MoonrakerRequestTracker::send() does.
     * @return Always returns 0 (success)
     */
    helix::RequestId send_jsonrpc(
        const std::string& method, const json& params, std::function<void(const json&)> success_cb,
        std::function<void(const MoonrakerError&)> error_cb, uint32_t timeout_ms = 0,
        bool silent = false,
        std::optional<helix::rpc_error_policy::CallerIntent> intent = std::nullopt) override;

    /**
     * @brief Simulate G-code script command
     *
     * Overrides base class to log and return success without network I/O.
     * Now properly returns errors for out-of-range moves (like real Klipper).
     *
     * @param gcode G-code string
     * @return 0 on success, non-zero on error (call get_last_gcode_error() for message)
     */
    int gcode_script(const std::string& gcode) override;

    /**
     * @brief Get the last G-code error message
     *
     * After gcode_script() returns non-zero, call this to get the error message.
     * Used by the RPC handler to return proper error responses.
     *
     * @return Error message, or empty string if no error
     */
    [[nodiscard]] std::string get_last_gcode_error() const;

    /**
     * @brief Set printer type for mock data generation
     *
     * @param type Printer type to simulate
     */
    void set_printer_type(PrinterType type) {
        printer_type_ = type;
    }

    /**
     * @brief Get the simulated printer type.
     *
     * Used by mock RPC handlers (e.g. printer.info) to report a printer-type
     * specific identity so PrinterDetector's fingerprint heuristics resolve the
     * mock to the matching printer_database.json entry.
     */
    PrinterType get_printer_type() const {
        return printer_type_;
    }

    /**
     * @brief Start temperature simulation loop
     *
     * Begins a background thread that simulates temperature changes
     * and pushes updates via notify_status_update callback.
     * Called automatically on connect().
     */
    void start_temperature_simulation();

    /**
     * @brief Stop temperature simulation loop
     *
     * Stops the background simulation thread.
     * Called automatically on disconnect() and destructor.
     *
     * @param during_destruction If true, skip logging (spdlog may be destroyed)
     */
    void stop_temperature_simulation(bool during_destruction = false);

    /**
     * @brief Set simulated extruder target temperature
     *
     * Starts heating/cooling simulation toward target.
     *
     * @param target Target temperature in Celsius
     */
    void set_extruder_target(double target);

    /**
     * @brief Set simulated bed target temperature
     *
     * Starts heating/cooling simulation toward target.
     *
     * @param target Target temperature in Celsius
     */
    void set_bed_target(double target);

    /**
     * @brief Set simulated chamber target temperature
     *
     * Starts heating/cooling simulation toward target.
     *
     * @param target Target temperature in Celsius
     */
    void set_chamber_target(double target);

    // ========== Test Helpers ==========

    /**
     * @brief Dispatch a method callback to registered handlers
     *
     * FOR TESTING ONLY. Invokes all registered callbacks for the given method
     * with the provided message. Used by unit tests to simulate WebSocket
     * notifications without actual network I/O.
     *
     * @param method Method name (e.g., "notify_gcode_response")
     * @param msg JSON message to pass to callbacks
     */
    void dispatch_method_callback(const std::string& method, const json& msg);

    /**
     * @brief Set heaters list for testing
     * @param heaters List of heater names (e.g., "extruder", "heater_bed")
     */
    void set_heaters(std::vector<std::string> heaters) {
        {
            std::lock_guard<std::mutex> lock(discovery_mutex_);
            discovery_.heaters() = std::move(heaters);
        }
        rebuild_hardware_from_lists();
    }

    /**
     * @brief Set fans list for testing
     * @param fans List of fan names (e.g., "fan", "heater_fan hotend_fan")
     */
    void set_fans(std::vector<std::string> fans) {
        {
            std::lock_guard<std::mutex> lock(discovery_mutex_);
            discovery_.fans() = std::move(fans);
        }
        rebuild_hardware_from_lists();
    }

    /**
     * @brief Set LEDs list for testing
     * @param leds List of LED names (e.g., "neopixel chamber_light")
     */
    void set_leds(std::vector<std::string> leds) {
        {
            std::lock_guard<std::mutex> lock(discovery_mutex_);
            discovery_.leds() = std::move(leds);
        }
        rebuild_hardware_from_lists();
    }

    /**
     * @brief Set sensors list for testing
     * @param sensors List of sensor names (e.g., "temperature_sensor chamber")
     */
    void set_sensors(std::vector<std::string> sensors) {
        {
            std::lock_guard<std::mutex> lock(discovery_mutex_);
            discovery_.sensors() = std::move(sensors);
        }
        rebuild_hardware_from_lists();
    }

    /**
     * @brief Set filament sensors list for testing
     * @param sensors List of filament sensor names (e.g., "filament_switch_sensor fsensor")
     */
    void set_filament_sensors(std::vector<std::string> sensors) {
        {
            std::lock_guard<std::mutex> lock(discovery_mutex_);
            discovery_.filament_sensors() = std::move(sensors);
        }
        rebuild_hardware_from_lists();
    }

    /**
     * @brief Set additional printer objects for testing (e.g., "mmu", "AFC", "toolchanger")
     *
     * These objects are included when populate_capabilities() is called, allowing
     * tests to configure capability flags like has_mmu() and has_tool_changer().
     *
     * @param objects List of additional Klipper object names to include
     */
    void set_additional_objects(std::vector<std::string> objects) {
        additional_objects_ = std::move(objects);
        rebuild_hardware_from_lists();
    }

    /**
     * @brief Set MMU availability for testing
     *
     * Controls whether the mock includes "mmu" in its printer objects,
     * which affects has_mmu() capability detection.
     *
     * @param enabled True to include MMU (default: true)
     */
    void set_mmu_enabled(bool enabled) {
        mmu_enabled_ = enabled;
        rebuild_hardware_from_lists();
    }

    /**
     * @brief Set accelerometer availability for testing
     *
     * When false, MEASURE_AXES_NOISE will dispatch an error response
     * simulating missing accelerometer configuration.
     *
     * @param available True if accelerometer should be available (default: true)
     */
    void set_accelerometer_available(bool available) {
        accelerometer_available_ = available;
    }

    /**
     * @brief Set input shaper configuration state for testing
     *
     * When false, get_input_shaper_config will return unconfigured state
     * (empty shaper types and zero frequencies).
     *
     * @param configured True if input shaper is configured (default: true)
     */
    void set_input_shaper_configured(bool configured) {
        input_shaper_configured_ = configured;
    }

    /**
     * @brief Stage the input_shaper values the mock reports (for testing)
     *
     * Overrides the default mzv@36.7 / ei@47.6 pair served while the input
     * shaper is configured, so a test can pin the live-before configuration
     * a calibration run snapshots. Applies to both the query and the
     * subscribe configfile payloads.
     *
     * @param type_x X shaper type (e.g. "ei")
     * @param freq_x X shaper frequency in Hz
     * @param type_y Y shaper type
     * @param freq_y Y shaper frequency in Hz
     */
    void set_input_shaper_values(const std::string& type_x, double freq_x,
                                 const std::string& type_y, double freq_y) {
        shaper_type_x_ = type_x;
        shaper_freq_x_ = freq_x;
        shaper_type_y_ = type_y;
        shaper_freq_y_ = freq_y;
    }

    /// The configfile "input_shaper" block as Klipper would report it (string
    /// values, configfile style). Shared by the query and subscribe handlers
    /// so both report identical staged values.
    [[nodiscard]] json build_input_shaper_config() const;

    /**
     * @brief Control whether SHAPER_CALIBRATE writes its CSV file for testing
     *
     * When false, the mock still dispatches the "calibration data written to
     * <path>" G-code line but does NOT create the file — simulating the case
     * where Klipper's /tmp output is unreadable (e.g. systemd PrivateTmp
     * isolation). Used to verify the chart-data-unavailable failure path.
     *
     * @param writable True to write the CSV (default: true)
     */
    void set_shaper_csv_writable(bool writable) {
        shaper_csv_writable_ = writable;
    }

    /**
     * @brief Set the [resonance_tester] sweep range the mock reports and replays
     *
     * Drives both `configfile.settings.resonance_tester` and the frequencies
     * the scripted SHAPER_CALIBRATE response actually sweeps, so a test can
     * reproduce a printer whose ceiling is not the value HelixScreen assumes.
     * Defaults mirror Kalico's out-of-the-box 5-135 Hz.
     */
    void set_resonance_sweep_range(double min_freq, double max_freq) {
        resonance_min_freq_ = min_freq;
        resonance_max_freq_ = max_freq;
    }

    [[nodiscard]] double get_resonance_min_freq() const {
        return resonance_min_freq_;
    }

    [[nodiscard]] double get_resonance_max_freq() const {
        return resonance_max_freq_;
    }

    /**
     * @brief Report stepper_z position_endstop as JSON null
     *
     * Klipper emits `position_endstop: null` for any printer configured with
     * `endstop_pin: probe:z_virtual_endstop` — i.e. most probe-equipped
     * machines. Lets tests reproduce that payload, which is otherwise
     * unrepresentable against the mock's hardcoded 235.0.
     *
     * @param null_endstop True to emit null instead of a number (default: false)
     */
    void set_stepper_z_endstop_null(bool null_endstop) {
        stepper_z_endstop_null_ = null_endstop;
    }

    /**
     * @brief Check whether stepper_z position_endstop is reported as null
     */
    [[nodiscard]] bool is_stepper_z_endstop_null() const {
        return stepper_z_endstop_null_;
    }

    /**
     * @brief Override the extruder max_temp reported in configfile.settings
     *
     * The default 300.0 sits below SafetyLimits::max_temperature_celsius (400.0),
     * so parsing it produces no observable change — which makes it useless as a
     * signal for "did the temperature-limit loop actually run?". Tests that need
     * that signal raise this above 400.0.
     *
     * @param max_temp Extruder max_temp in celsius (default: 300.0)
     */
    void set_extruder_max_temp(double max_temp) {
        extruder_max_temp_ = max_temp;
    }

    /**
     * @brief Get the extruder max_temp reported in configfile.settings
     */
    [[nodiscard]] double get_extruder_max_temp() const {
        return extruder_max_temp_;
    }

    /**
     * @brief Check if mock accelerometer is enabled
     * @return true if accelerometer should be reported as available
     */
    [[nodiscard]] bool is_accelerometer_available() const {
        return accelerometer_available_;
    }

    /**
     * @brief Check if mock input shaper is configured
     * @return true if input shaper should be reported as configured
     */
    [[nodiscard]] bool is_input_shaper_configured() const {
        return input_shaper_configured_;
    }

    /**
     * @brief Check if mock Spoolman is enabled
     *
     * Controlled by HELIX_MOCK_SPOOLMAN env var (default: true).
     * Set to "0" or "off" to disable.
     *
     * @return true if Spoolman should be reported as available during discovery
     */
    [[nodiscard]] bool is_mock_spoolman_enabled() const {
        return mock_spoolman_enabled_;
    }

    // ========== Internal API (for use by method handler modules) ==========

    /**
     * @brief Start a print job (internal implementation)
     *
     * Extracts metadata from the G-code file and begins preheat phase.
     * Called by both SDCARD_PRINT_FILE G-code and printer.print.start JSON-RPC.
     *
     * @param filename G-code filename (relative path)
     * @return true if print started successfully, false on error
     */
    bool start_print_internal(const std::string& filename);

    /**
     * @brief Pause current print (internal implementation)
     *
     * Called by both PAUSE G-code and printer.print.resume JSON-RPC.
     *
     * @return true if print was paused, false if not currently printing
     */
    bool pause_print_internal();

    /**
     * @brief Resume paused print (internal implementation)
     *
     * Called by both RESUME G-code and printer.print.resume JSON-RPC.
     *
     * @return true if print was resumed, false if not currently paused
     */
    bool resume_print_internal();

    /**
     * @brief Cancel current print (internal implementation)
     *
     * Called by both CANCEL_PRINT G-code and printer.print.cancel JSON-RPC.
     *
     * @return true if print was cancelled, false if no active print
     */
    bool cancel_print_internal();

    /**
     * @brief Execute emergency stop (internal implementation)
     *
     * Zeros all heater targets and fan speeds, sets print state to error,
     * and transitions klippy state to SHUTDOWN. Called by both the
     * printer.emergency_stop JSON-RPC handler and M112 G-code.
     */
    void emergency_stop_internal();

    /**
     * @brief Toggle filament runout state for simulation
     *
     * Toggles the filament_detected state on the primary runout sensor and
     * dispatches a status update. Useful for testing filament runout handling.
     *
     * @return true if toggled, false if no filament sensors configured
     */
    bool toggle_filament_runout();

    /**
     * @brief Override base class simulation method
     *
     * Delegates to toggle_filament_runout() to avoid layer violation
     * where Application would need to cast to MoonrakerClientMock.
     */
    void toggle_filament_runout_simulation() override {
        toggle_filament_runout();
    }

    // ========== Bed Mesh Accessors (Mock-specific) ==========
    // These were removed from MoonrakerClient and moved to MoonrakerAPI.
    // The mock needs its own accessors for test validation.

    /**
     * @brief Get the active bed mesh profile
     * @return Reference to the currently active bed mesh profile
     */
    [[nodiscard]] const BedMeshProfile& get_active_bed_mesh() const {
        return active_bed_mesh_;
    }

    /**
     * @brief Get list of available bed mesh profile names
     * @return Vector of profile names
     */
    [[nodiscard]] const std::vector<std::string>& get_bed_mesh_profiles() const {
        return bed_mesh_profiles_;
    }

    /**
     * @brief Check if bed mesh data is available
     * @return true if a valid bed mesh profile has been loaded
     */
    [[nodiscard]] bool has_bed_mesh() const {
        return !active_bed_mesh_.probed_matrix.empty();
    }

    /**
     * @brief Serve configfile.settings.bed_mesh.probe_count from objects.query
     *
     * Mirrors what a printer with a configured [bed_mesh] section answers —
     * the print-start collector's entry-time query reads it to size the mesh
     * point denominator. Unset by default so existing tests see the same
     * objects.query responses as before.
     */
    void set_config_bed_mesh_probe_count(int first, int second) {
        config_bed_mesh_probe_count_ = std::make_pair(first, second);
    }

    /// The configured probe_count pair, or nullptr when unset.
    [[nodiscard]] const std::pair<int, int>* config_bed_mesh_probe_count() const {
        return config_bed_mesh_probe_count_ ? &*config_bed_mesh_probe_count_ : nullptr;
    }

    // ========================================================================
    // Test Helpers - Public dispatch for test injection
    // ========================================================================

    /**
     * @brief Dispatch a gcode response line to all registered callbacks
     *
     * Public for test code to inject arbitrary gcode response lines,
     * simulating Klipper output for collector-based APIs.
     *
     * @param line G-code response line to dispatch
     */
    void dispatch_gcode_response(const std::string& line);

    /**
     * @brief Append chamber-backend status objects to a synthesized frame
     *
     * Emits the nominal diagnostics payload + filter-pin value for appliance
     * chamber-heater backends (e.g. dragonbreath) whose objects were
     * materialized via HELIX_MOCK_OBJECTS. `requested` (a
     * printer.objects.subscribe objects filter) limits emission to subscribed
     * keys; null emits every present backend object.
     */
    void append_chamber_backend_status(json& status_obj, double sim_time,
                                       const json* requested = nullptr) const;

    /**
     * @brief Bare gcode name of the resolved chamber heater
     * @return e.g. "chamber" or "dragonbreath" (the HEATER= form), empty if none
     */
    std::string chamber_heater_bare_name() const;

    /**
     * @brief Full filter-fan pin object of the chamber backend, when materialized
     * @return e.g. "output_pin dragonbreath_filter", empty when absent
     */
    std::string chamber_filter_pin_object() const;

  private:
    // Test visibility into the chamber-key cache and the synchronous initial
    // state dispatch (see tests/test_helpers/moonraker_client_mock_test_access.h).
    friend class helix::MoonrakerClientMockTestAccess;

    /**
     * @brief Populate hardware lists based on configured printer type
     *
     * Populates hardware vectors in discovery_ sequence
     * (heaters, sensors, fans, leds) based on configured printer type.
     */
    void populate_hardware();

    /**
     * @brief Populate capabilities from mock Klipper object list
     *
     * Creates mock objects including macros, probes, bed mesh config, etc.
     * Called from constructor so capabilities are available immediately,
     * and also from discover_printer() for consistency.
     */
    void populate_capabilities();

    /**
     * @brief Detect toolchanger mock mode from HELIX_MOCK_AMS env var
     *
     * Mirrors the parsing in ams_backend.cpp: returns true when HELIX_MOCK_AMS
     * (lowercased) is one of "toolchanger", "tool_changer", or "tc". Used to gate
     * the multi-extruder toolchanger emulation (4 distinct extruder heaters,
     * toolchanger + tool objects) so single-extruder/AFC/MMU/mixed/IFS modes are
     * unaffected.
     */
    bool is_mock_toolchanger() const;

    /**
     * @brief Rebuild hardware from current discovery lists (heaters, fans, sensors, etc.)
     *
     * Lightweight re-parse used by test helpers (set_heaters, set_fans, etc.)
     * to update hardware() after modifying discovery lists. Unlike populate_capabilities(),
     * this does NOT add hardcoded common objects — it only uses what's in the lists.
     */
    void rebuild_hardware_from_lists();

    /**
     * @brief Generate synthetic bed mesh data for testing
     *
     * Creates a realistic dome-shaped mesh (7×7 points, 0-0.3mm Z range).
     * Populates active_bed_mesh_ with test data compatible with renderer.
     */
    void generate_mock_bed_mesh();

    /**
     * @brief Parse incoming bed mesh JSON from dispatch_status_update
     *
     * Updates active_bed_mesh_ from status notification data. This allows
     * tests to inject custom bed mesh configurations via dispatch_status_update.
     *
     * @param bed_mesh JSON object containing bed_mesh status fields
     */
    void parse_incoming_bed_mesh(const json& bed_mesh);

    /**
     * @brief Generate bed mesh with slight variation
     *
     * Used by BED_MESH_CALIBRATE simulation to create new mesh data with
     * small variations, simulating the behavior of re-probing the bed.
     * Variation is deterministic based on profile name.
     */
    void generate_mock_bed_mesh_with_variation();

    /**
     * @brief Dispatch bed mesh update notification
     *
     * Sends a notify_status_update with the current bed mesh state.
     * Called after BED_MESH_CALIBRATE, BED_MESH_PROFILE, or BED_MESH_CLEAR.
     */
    void dispatch_bed_mesh_update();

    /**
     * @brief Temperature simulation loop (runs in background thread)
     */
    void temperature_simulation_loop();

    /**
     * @brief Dispatch historical temperature data at startup
     *
     * Generates 2-3 minutes of synthetic temperature readings and dispatches
     * them rapidly to observers. This populates the temperature graph with
     * realistic-looking data immediately upon connection.
     */
    void dispatch_historical_temperatures();

    /**
     * @brief Dispatch initial printer state to observers
     *
     * Called during connect() to send initial state, matching the behavior
     * of the real MoonrakerClient which sends initial state from the
     * subscription response. Uses dispatch_status_update() from base class.
     */
    void dispatch_initial_state();

    /**
     * @brief Get print state as string for Moonraker-compatible notifications
     *
     * @return String representation: "standby", "printing", "paused", "complete", "cancelled",
     * "error"
     */
    std::string get_print_state_string() const;

    // ========== Simulation Helpers ==========

    /**
     * @brief Check if temperature has reached target within tolerance
     * @param current Current temperature
     * @param target Target temperature
     * @param tolerance Acceptable difference (default 2°C)
     * @return true if within tolerance
     */
    bool is_temp_stable(double current, double target, double tolerance = 2.0) const;

    /**
     * @brief Check if this printer profile has a chamber sensor
     * @return true if "temperature_sensor chamber" is in sensors list
     */
    bool has_chamber_sensor() const;

    /**
     * @brief Get the Klipper object name used for chamber heater status updates
     * @return e.g., "heater_generic chamber" or "temperature_fan chamber", empty if none
     */
    std::string chamber_heater_status_key() const;

    /**
     * @brief Replace existing chamber heaters in discovery lists with the given object
     *
     * Removes any heater containing "chamber" from discovery_.heaters() and adds
     * the replacement. For temperature_fan types, also adds to sensors list.
     */
    void override_chamber_heater(const std::string& heater_obj);

    /// Refresh cached_chamber_status_key_ from current heaters list
    void update_cached_chamber_key();

    /**
     * @brief Advance print progress based on simulated time elapsed
     * @param dt_simulated Simulated time step in seconds (affected by speedup)
     */
    void advance_print_progress(double dt_simulated);

    /**
     * @brief Dispatch enhanced print status notification
     *
     * Sends Moonraker-compatible notification with full print_stats
     * and virtual_sdcard objects.
     */
    void dispatch_enhanced_print_status();

    /**
     * @brief Dispatch print state change notification
     * @param state New state string ("printing", "paused", "complete", etc.)
     */
    void dispatch_print_state_notification(const std::string& state);

    /**
     * @brief Trigger Klipper restart simulation
     *
     * Sets klippy_state to STARTUP, clears active print, sets heater targets to 0,
     * then spawns a thread to restore READY state after delay. Temps continue
     * cooling naturally during the restart period.
     *
     * @param is_firmware true for FIRMWARE_RESTART (3s), false for RESTART (2s)
     */
    void trigger_restart(bool is_firmware);

    /**
     * @brief Set fan speed internally and dispatch status update
     * @param fan_name Full fan name (e.g., "fan", "fan_generic nevermore")
     * @param speed Normalized speed 0.0-1.0
     */
    void set_fan_speed_internal(const std::string& fan_name, double speed);

    /**
     * @brief Find fan by suffix match in discovered fans
     * @param suffix Fan name suffix (e.g., "nevermore" matches "fan_generic nevermore")
     * @return Full fan name, or empty string if not found
     */
    std::string find_fan_by_suffix(const std::string& suffix) const;

    /**
     * @brief Dispatch gcode_move status update (for Z offset changes)
     */
    void dispatch_gcode_move_update();

    /**
     * @brief Dispatch manual_probe status update (for Z-offset calibration)
     */
    void dispatch_manual_probe_update();

    /**
     * @brief Read all three position axes as a consistent snapshot
     *
     * Acquires pos_mutex_ so the returned (x, y, z) triple is never torn across
     * a concurrent multi-axis move. Releases the lock before returning, keeping
     * pos_mutex_ a leaf lock.
     */
    void read_position_snapshot(double& x, double& y, double& z) const;

    /**
     * @brief Dispatch SHAPER_CALIBRATE response sequence
     *
     * Simulates the G-code response output from Klipper's SHAPER_CALIBRATE
     * command, including fitted shaper results and recommendation.
     * Used to enable input shaper calibration tests.
     *
     * @param axis Axis being calibrated ('X' or 'Y')
     */
    void dispatch_shaper_calibrate_response(char axis);

    /**
     * @brief Dispatch MEASURE_AXES_NOISE response
     *
     * Simulates the G-code response output from Klipper's MEASURE_AXES_NOISE
     * command, returning a mock accelerometer noise level.
     * Used to enable input shaper noise check tests.
     */
    void dispatch_measure_axes_noise_response();

    /**
     * @brief Advance PRINT_START simulation based on temperature progress
     *
     * Called during PREHEAT phase to dispatch simulated G-code responses
     * for common PRINT_START phases (homing, heating, QGL, etc.) based on
     * temperature progress toward targets.
     */
    void advance_print_start_simulation();

    /**
     * @brief Generate next mock request ID
     * @return Valid request ID (always > 0)
     */
    helix::RequestId next_mock_request_id() {
        return mock_request_id_counter_.fetch_add(1) + 1;
    }

  public:
    /**
     * @brief Build a realistic temperature history as a server.temperature_store payload
     *
     * Generates ~10 minutes of 1 Hz per-sensor history with a heat-up ->
     * hold-at-target -> cooldown profile (extruder 25->215C, bed 25->60C) plus
     * gentle sinusoids for any discovered temperature_sensor. The curve ends
     * near ambient so it joins the idle live simulation smoothly. Used by the
     * mock server.temperature_store handler so the connect-time seed populates
     * the graphs in mock mode (#944).
     */
    TemperatureStore build_historical_temperature_store() const;

    /// Test inspection: the most recent timeout_ms passed to the 5-arg send_jsonrpc().
    /// 0 means either "not yet called" or "caller relied on default".
    uint32_t last_send_timeout_ms() const {
        return last_send_timeout_ms_;
    }

    /// Test inspection: the most recent silent flag passed to the 5-arg send_jsonrpc().
    bool last_send_silent() const {
        return last_send_silent_;
    }

    /// Error-reporting intent of the send currently being dispatched into the
    /// method-handler registry. Those handlers run inline inside send_jsonrpc()
    /// and never reach MoonrakerRequestTracker, so they read this to make the
    /// same helix::rpc_error_policy::decide() call the tracker makes on real
    /// hardware — otherwise mock and hardware drift on who reports an error.
    /// It keeps the last dispatched value, so tests can also read it after the
    /// call to assert what intent a caller declared.
    const helix::rpc_error_policy::CallerIntent& current_send_intent() const {
        return current_send_intent_;
    }

    /// Test inspection: the most recent RPC method name passed to the 5-arg send_jsonrpc().
    const std::string& last_send_method() const {
        return last_send_method_;
    }

    /// Test inspection: the `script` param of the most recent printer.gcode.script RPC
    /// passed to the 5-arg send_jsonrpc(). Empty until the first such call. Used by
    /// tests that verify the exact G-code a caller routes through the send chokepoint.
    const std::string& last_send_script() const {
        return last_send_script_;
    }

    /// Test helper: force the next matching printer.gcode.script RPC to invoke its
    /// error callback (instead of success) with the given error type/message. This
    /// simulates an RPC-layer timeout while Klipper still processes the gcode — the
    /// command's normal side effects still run; only the RPC response is replaced.
    /// One-shot: cleared once it fires. When @p script_substr is non-empty, only a
    /// gcode.script whose script contains that substring triggers the forced error.
    void force_next_gcode_error(MoonrakerErrorType type, const std::string& message,
                                const std::string& script_substr = "") {
        std::lock_guard<std::mutex> lock(forced_gcode_error_mutex_);
        forced_gcode_error_ = ForcedGcodeError{type, message, script_substr};
    }

    /// Consume a pending forced gcode error if it matches @p script. Returns the
    /// error to fire (and clears the pending state), or nullopt if none is pending
    /// or the script filter does not match. Used by the gcode.script mock handler.
    std::optional<MoonrakerError> take_forced_gcode_error(const std::string& script) {
        std::lock_guard<std::mutex> lock(forced_gcode_error_mutex_);
        if (!forced_gcode_error_.has_value())
            return std::nullopt;
        if (!forced_gcode_error_->script_substr.empty() &&
            script.find(forced_gcode_error_->script_substr) == std::string::npos)
            return std::nullopt;
        MoonrakerError err;
        err.type = forced_gcode_error_->type;
        err.message = forced_gcode_error_->message;
        err.method = "printer.gcode.script";
        forced_gcode_error_.reset();
        return err;
    }

    /// Test helper: force the next matching printer.gcode.script RPC to invoke NEITHER
    /// callback — the request is sent, Klipper still runs the gcode, but the RPC
    /// response never arrives. This is the real-world wedge #1129 reported: a Klippy
    /// restart (or any dropped response) leaves a caller's in-flight counter pinned
    /// because neither on_success nor on_error ever fires. One-shot: cleared once it
    /// fires. When @p script_substr is non-empty, only a gcode.script whose script
    /// contains that substring has its response dropped.
    void force_next_gcode_dropped_response(const std::string& script_substr = "") {
        std::lock_guard<std::mutex> lock(forced_gcode_error_mutex_);
        forced_gcode_drop_ = script_substr;
    }

    /// Consume a pending forced response-drop if it matches @p script. Returns true
    /// (and clears the pending state) when the handler must return without invoking
    /// either callback. Used by the gcode.script mock handler.
    bool take_forced_gcode_drop(const std::string& script) {
        std::lock_guard<std::mutex> lock(forced_gcode_error_mutex_);
        if (!forced_gcode_drop_.has_value())
            return false;
        if (!forced_gcode_drop_->empty() && script.find(*forced_gcode_drop_) == std::string::npos)
            return false;
        forced_gcode_drop_.reset();
        return true;
    }

    /// Test inspection: every gcode script string passed through the
    /// printer.gcode.script handler, in order. Lets tests assert multi-step
    /// gcode sequences (e.g. the #991 split config-reassert + heat/feed chain).
    const std::vector<std::string>& gcode_script_history() const {
        std::lock_guard<std::mutex> lock(gcode_history_mutex_);
        return gcode_script_history_;
    }

    /// Test helper: clear the recorded gcode-script history.
    void clear_gcode_script_history() {
        std::lock_guard<std::mutex> lock(gcode_history_mutex_);
        gcode_script_history_.clear();
    }

    /// Record a gcode script for test inspection. Called by gcode_script().
    void record_gcode_script(const std::string& script) {
        std::lock_guard<std::mutex> lock(gcode_history_mutex_);
        gcode_script_history_.push_back(script);
    }

  private:
    PrinterType printer_type_;

    // Test inspection: ordered history of every gcode script handled.
    std::vector<std::string> gcode_script_history_;
    mutable std::mutex gcode_history_mutex_;

    // Mock bed mesh storage (Client no longer stores this; mock simulates it)
    BedMeshProfile active_bed_mesh_;

    /// configfile.settings.bed_mesh.probe_count to serve from objects.query
    /// when set via set_config_bed_mesh_probe_count(); unset = absent.
    std::optional<std::pair<int, int>> config_bed_mesh_probe_count_;
    std::vector<std::string> bed_mesh_profiles_;
    std::map<std::string, BedMeshProfile> stored_bed_mesh_profiles_; // Actual mesh data per profile

    // Mock request ID counter for simulating send_jsonrpc return values
    std::atomic<helix::RequestId> mock_request_id_counter_{0};

    // Test inspection: last send_jsonrpc(method,params,succ,err,timeout_ms,silent) args.
    std::string last_send_method_;
    std::string last_send_script_; // `script` param when method == printer.gcode.script
    uint32_t last_send_timeout_ms_{0};
    bool last_send_silent_{false};

    // Intent of the in-flight send_jsonrpc() dispatch, read by the method
    // handlers via current_send_intent().
    helix::rpc_error_policy::CallerIntent current_send_intent_{};

    // One-shot forced error injection for printer.gcode.script (test helper).
    struct ForcedGcodeError {
        MoonrakerErrorType type;
        std::string message;
        std::string script_substr;
    };
    std::optional<ForcedGcodeError> forced_gcode_error_;
    // One-shot forced response-drop for printer.gcode.script (test helper). Holds the
    // script substring filter; empty string matches any script. Shares the mutex above.
    std::optional<std::string> forced_gcode_drop_;
    mutable std::mutex forced_gcode_error_mutex_;

    // Temperature simulation state
    std::atomic<double> extruder_temp_{25.0};  // Current temperature
    std::atomic<double> extruder_target_{0.0}; // Target temperature (0 = off)
    std::atomic<double> bed_temp_{25.0};       // Current temperature
    std::atomic<double> bed_target_{0.0};      // Target temperature (0 = off)
    std::atomic<double> chamber_temp_{25.0};   // Chamber temp (25-45°C, passive sensor)
    std::atomic<double> chamber_target_{0.0};  // Chamber target temperature (0 = off)
    std::atomic<double> mcu_temp_{42.0};       // MCU temp (40-55°C, stable with small variation)
    std::atomic<double> host_temp_{52.0};      // Host/RPi temp (45-65°C, correlates with load)

    // Position simulation state.
    // The three axes are individually atomic, but a multi-axis move and the
    // background simulation loop's status broadcast run on different threads.
    // pos_mutex_ makes a multi-axis write and a snapshot read atomic *as a
    // group*, so a broadcast can never observe a torn cross-axis snapshot (e.g.
    // X/Y updated but Z still stale) mid-move. It is a leaf lock: never acquire
    // another lock while holding it. Use read_position_snapshot() to read all
    // three together; wrap multi-axis stores in a lock_guard(pos_mutex_).
    mutable std::mutex pos_mutex_;
    std::atomic<double> pos_x_{0.0};
    std::atomic<double> pos_y_{0.0};
    std::atomic<double> pos_z_{0.0};

    // Motion mode state
    std::atomic<bool> relative_mode_{false}; // G90=absolute (false), G91=relative (true)
    std::atomic<bool> motors_enabled_{true}; // Track motor enable state for idle_timeout

    // Idle timeout simulation.
    // Atomic because reset_idle_timeout() writes it from whichever thread drove
    // the activity while temperature_simulation_loop() reads it on the sim
    // thread; the siblings below were already atomic and this one was missed.
    std::atomic<std::chrono::steady_clock::time_point> last_activity_time_;
    std::atomic<bool> idle_timeout_triggered_{false};
    std::atomic<uint32_t> idle_timeout_seconds_{600}; // Default 10 minutes

    // Homing state (needs mutex since std::string is not atomic)
    mutable std::mutex homed_axes_mutex_;
    std::string homed_axes_;

    // G-code error tracking (for RPC handler to return proper errors)
    mutable std::mutex gcode_error_mutex_;
    std::string last_gcode_error_;

    // M117 display message (needs mutex since std::string is not atomic).
    // display_message_set_ distinguishes "user sent M117" (even to clear the
    // text to "") from "no M117 has been sent yet" so the periodic status
    // broadcast knows whether to keep showing the canned phase strings
    // ("Heating...", "Purging nozzle") or the user's message.
    mutable std::mutex display_message_mutex_;
    std::string display_message_;
    bool display_message_set_ = false;

    // Print simulation state (legacy - kept for backward compatibility)
    std::atomic<int> print_state_{
        0}; // 0=standby, 1=printing, 2=paused, 3=complete, 4=cancelled, 5=error
    std::string print_filename_;              // Current print file (protected by print_mutex_)
    mutable std::mutex print_mutex_;          // Protects print_filename_
    std::atomic<double> print_progress_{0.0}; // 0.0 to 1.0
    std::atomic<int> speed_factor_{100};      // Percentage
    std::atomic<int> flow_factor_{100};       // Percentage
    std::atomic<int> fan_speed_{0};           // 0-255

    // Enhanced print simulation state (phase-based)
    std::atomic<MockPrintPhase> print_phase_{MockPrintPhase::IDLE};
    MockPrintMetadata print_metadata_;        // Current print job metadata
    mutable std::mutex metadata_mutex_;       // Protects print_metadata_
    std::atomic<double> speedup_factor_{1.0}; // Simulation speedup (1.0 = real-time)

    // Dominant gcode tool (highest filament weight). Mock-side proxy for what
    // Klipper would publish via printer.mmu.tool / toolchanger.tool_number.
    // -1 when no print active or slicer didn't emit per-tool data.
    std::atomic<int> active_gcode_tool_{-1};
    // Per-tool slicer colors parsed from the gcode header. Used by observers
    // as a fallback color when the active tool isn't mapped to a real slot
    // — gives the swatch a meaningful color instead of slot 0's default.
    std::vector<uint32_t> active_gcode_tool_colors_;
    mutable std::mutex active_gcode_tool_mutex_;
    std::vector<std::function<void(int, uint32_t)>> active_gcode_tool_observers_;

  public:
    /**
     * @brief Register a callback fired when the simulated print's active gcode
     * tool changes. Used by AmsBackendMock to follow tool state.
     *
     * Args: (tool_index, slicer_color_rgb). slicer_color_rgb is 0 when no
     * per-tool color metadata was emitted by the slicer. Callback may fire
     * from any thread; receiver is responsible for marshalling.
     */
    void add_active_gcode_tool_observer(std::function<void(int, uint32_t)> cb) {
        std::lock_guard<std::mutex> lock(active_gcode_tool_mutex_);
        active_gcode_tool_observers_.push_back(std::move(cb));
    }

    /**
     * @brief Current active gcode tool index (-1 if none / no per-tool data).
     */
    [[nodiscard]] int get_active_gcode_tool() const {
        return active_gcode_tool_.load();
    }

  private:
    void notify_active_gcode_tool_observers(int tool) {
        std::vector<std::function<void(int, uint32_t)>> snapshot;
        uint32_t color = 0;
        {
            std::lock_guard<std::mutex> lock(active_gcode_tool_mutex_);
            snapshot = active_gcode_tool_observers_;
            if (tool >= 0 && tool < static_cast<int>(active_gcode_tool_colors_.size())) {
                color = active_gcode_tool_colors_[tool];
            }
        }
        for (auto& cb : snapshot) {
            if (cb) {
                cb(tool, color);
            }
        }
    }

    // Print timing (wall-clock for internal tracking)
    std::optional<std::chrono::steady_clock::time_point> preheat_start_time_;
    std::optional<std::chrono::steady_clock::time_point> printing_start_time_;
    std::chrono::steady_clock::time_point pause_start_time_;
    double total_pause_duration_sim_{0.0}; // Accumulated pause time in simulated seconds

    // LED simulation state (RGBW values 0.0-1.0)
    struct LedColor {
        double r = 0.0, g = 0.0, b = 0.0, w = 0.0;
    };
    std::map<std::string, LedColor> led_states_; // LED name -> color
    mutable std::mutex led_mutex_;               // Protects led_states_

    // Klippy service state (for RESTART/FIRMWARE_RESTART simulation)
    std::atomic<KlippyState> klippy_state_{KlippyState::READY};

    // Fan speed tracking (multiple fans by name)
    std::map<std::string, double> fan_speeds_; // Fan name -> speed (0.0-1.0)
    mutable std::mutex fan_mutex_;             // Protects fan_speeds_

    // G-code offset tracking
    std::atomic<double> gcode_offset_z_{0.0}; // Z offset from SET_GCODE_OFFSET

    // Manual probe state (for Z-offset calibration: PROBE_CALIBRATE, TESTZ, ACCEPT, ABORT)
    std::atomic<bool> manual_probe_active_{false}; // true when in probe mode
    std::atomic<double> manual_probe_z_{0.0};      // Current Z position during calibration

    // Excluded objects tracking (for EXCLUDE_OBJECT command)
    std::set<std::string> excluded_objects_; // Object names excluded during print (local fallback)
    std::vector<std::string> object_names_;  // Defined object names from gcode (local fallback)
    mutable std::mutex excluded_objects_mutex_; // Protects excluded_objects_ and object_names_

    /// How many synthetic exclude_object entries to publish at print start, or 0
    /// to publish only what the G-code declares. Set by HELIX_MOCK_EXCLUDE_OBJECTS.
    int mock_exclude_object_count_{0};

    // Shared mock state for coordination with MoonrakerAPIMock
    // When set, state changes are propagated to this shared object
    std::shared_ptr<MockPrinterState> mock_state_;

    // Simulation tick counter
    std::atomic<uint32_t> tick_count_{0};

    // Filament runout simulation state
    std::atomic<bool> filament_runout_state_{true}; // true = filament detected

    // PRINT_START simulation phases (for G-code response notifications)
    // Tracks which phases have already been dispatched during current print
    enum class SimulatedPrintStartPhase : uint8_t {
        NONE = 0,
        PRINT_START_MARKER = 1, // "PRINT_START" detected
        HOMING = 2,             // "G28" dispatched
        HEATING_BED = 3,        // "M190 S60" dispatched
        HEATING_NOZZLE = 4,     // "M109 S210" dispatched
        QGL = 5,                // "QUAD_GANTRY_LEVEL" dispatched
        BED_MESH = 6,           // "BED_MESH_CALIBRATE" dispatched
        PURGING = 7,            // "VORON_PURGE" dispatched
        LAYER_1 = 8             // "SET_PRINT_STATS_INFO CURRENT_LAYER=1" dispatched
    };
    std::atomic<uint8_t> simulated_print_start_phase_{0};

    // Simulation thread control
    std::thread simulation_thread_;
    std::atomic<bool> simulation_running_{false};
    std::mutex sim_mutex_;           // For condition variable wait
    std::condition_variable sim_cv_; // For interruptible sleep during shutdown

    // Protects the discovery_ name lists (heaters/fans/sensors/leds/filament_sensors)
    // against the temperature simulation thread.
    //
    // connect() starts that thread, and discover_printer() then REASSIGNS those
    // vectors on the calling thread — `discovery_.fans() = {...}` frees every old
    // std::string buffer while the simulation loop is iterating them. ASan caught
    // this as two heap-use-after-frees (memcpy and memcmp on freed string data)
    // from FullStackTestFixture, whose constructor does exactly that sequence.
    //
    // Lock discipline, verified against the call graph — do not nest these:
    //   LOCKED   populate_hardware(), populate_capabilities(),
    //            rebuild_hardware_from_lists(), the discovery-list setters below
    //            (assignment only), the simulation loop's per-iteration snapshot,
    //            and has_chamber_sensor() — the loop's third read path, which
    //            reaches discovery_.sensors() through a call rather than inline
    //   UNLOCKED override_chamber_heater() — reached ONLY from populate_capabilities()
    //            and rebuild_hardware_from_lists(), both of which already hold it
    mutable std::mutex discovery_mutex_;

    // Restart simulation thread (for RESTART/FIRMWARE_RESTART commands)
    std::thread restart_thread_;
    std::atomic<bool> restart_pending_{false};
    mutable std::mutex restart_mutex_; // Protects restart_thread_ lifecycle

    // Method handler registry (populated at construction)
    std::unordered_map<std::string, mock_internal::MethodHandler> method_handlers_;

    // Simulation parameters (realistic heating rates)
    static constexpr double ROOM_TEMP = 25.0;
    static constexpr double EXTRUDER_HEAT_RATE = 3.0;     // °C/sec when heating
    static constexpr double EXTRUDER_COOL_RATE = 1.5;     // °C/sec when cooling
    static constexpr double BED_HEAT_RATE = 1.0;          // °C/sec when heating
    static constexpr double BED_COOL_RATE = 0.3;          // °C/sec when cooling
    static constexpr double CHAMBER_HEAT_RATE = 0.3;      // °C/sec when heating (slower than bed)
    static constexpr double CHAMBER_COOL_RATE = 0.1;      // °C/sec when cooling
    static constexpr int SIMULATION_INTERVAL_MS = 250;    // Physics tick interval
    static constexpr int NOTIFICATION_INTERVAL_TICKS = 4; // Dispatch every 4 ticks (~1s)

    // Mock service availability flags (initialized from env vars in constructor)
    bool mock_spoolman_enabled_{true};   ///< Controlled by HELIX_MOCK_SPOOLMAN env var
    bool accelerometer_available_{true}; ///< Accelerometer available for input shaper tests
    bool input_shaper_configured_{true}; ///< Input shaper configured for config query tests
    /// Staged input shaper values served while configured (see
    /// set_input_shaper_values); defaults mirror the mock's original payload.
    std::string shaper_type_x_{"mzv"};
    double shaper_freq_x_{36.7};
    std::string shaper_type_y_{"ei"};
    double shaper_freq_y_{47.6};
    bool shaper_csv_writable_{true};     ///< When false, SHAPER_CALIBRATE skips writing the CSV
    bool stepper_z_endstop_null_{false}; ///< When true, position_endstop is reported as JSON null
    double extruder_max_temp_{300.0};    ///< Extruder max_temp reported in configfile.settings
    double resonance_min_freq_{5.0};     ///< [resonance_tester] min_freq the mock reports/sweeps
    double resonance_max_freq_{135.0};   ///< [resonance_tester] max_freq the mock reports/sweeps
    bool mmu_enabled_{true};             ///< MMU available (default true for existing tests)

    // Additional objects for testing (e.g., "mmu", "AFC", "toolchanger")
    std::vector<std::string> additional_objects_;

    // Cached chamber heater status key (updated by override_chamber_heater / populate)
    std::string cached_chamber_status_key_;

    // Chamber filter-fan pin value (SET_PIN target; drives fan_percent in
    // synthesized chamber-backend diagnostics frames)
    std::atomic<double> chamber_filter_value_{0.0};

    // Calibration simulation timers (PID, MPC, shaper) — must be cleaned up
    // in destructor to prevent use-after-free when mock is destroyed before
    // LVGL timers fire in a subsequent test's process_lvgl(). Each entry also
    // owns its heap payload: the timer callbacks self-delete at natural
    // completion, but a forced teardown that only lv_timer_delete()s the timer
    // leaks the payload (the 80-byte ShaperSimState the 2026-08-18 nightly's
    // leak report showed surviving the suite).
    struct CalibrationTimer {
        lv_timer_t* timer = nullptr;
        std::function<void()> free_payload;
    };
    std::vector<CalibrationTimer> calibration_timers_;

    // Capture replay (arm_event_replay): timed events walked through the real
    // dispatch paths once connect() finishes its initial state.
    struct ReplayEvent {
        uint64_t t_ms = 0;
        bool is_gcode = false;
        std::string line;       // gcode_response text (with // prefix)
        std::string object;     // status object name
        nlohmann::json payload; // status payload
    };
    std::vector<ReplayEvent> replay_events_;
    size_t replay_next_ = 0;
    lv_timer_t* replay_timer_ = nullptr;
    std::chrono::steady_clock::time_point replay_start_{};

    void start_replay_timer();
    void pump_replay();
    void fire_replay_event(const ReplayEvent& event);
};

// ============================================================================
// Test Utility Functions
// ============================================================================

/**
 * @brief Simulate USB symlink presence for testing
 *
 * When active, list_files("gcodes", "usb") returns mock files instead of empty.
 * Used to test USB symlink detection in PrintSelectPanel.
 *
 * @param active True to simulate symlink exists, false for no symlink
 */
void mock_set_usb_symlink_active(bool active);
