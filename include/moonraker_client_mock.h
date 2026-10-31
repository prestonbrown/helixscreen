// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "accel_batch_source_mock.h"
#include "async_lifetime_guard.h"
#include "axis.h"
#include "helix_shared_callback.h"
#include "mock_persona.h"
#include "moonraker_client.h"
#include "moonraker_client_mock_spoolman.h"
#include "moonraker_types.h"
#include "preprint_skip_wrappers.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
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

// sim_speed() returns this by value, which a declaration may do with an
// incomplete type. Forward-declaring rather than including keeps
// simulated_clock.h off the ~140 translation units that pull this header in
// for the mock and never touch the speed scale; sim_speed()'s definition and
// its callers include it themselves.
namespace helix::sim {
class SimSpeed;
} // namespace helix::sim

// Forward declaration for internal handler registry
namespace mock_internal {
using MethodHandler =
    std::function<bool(MoonrakerClientMock*, const json&, helix::SharedCallback<void(const json&)>,
                       helix::SharedCallback<void(const MoonrakerError&)>)>;
} // namespace mock_internal

/// Failure modes the TEST_RESONANCES simulation can be switched to.
enum class BeltMockFailure { // NAMESPACE_OK: sits beside MoonrakerClientMock, this header's
                             // global-scope mock API
    NONE,                    ///< Normal sweep: console lines plus a written CSV
    STALL,                   ///< Sweep dies at its midpoint; no file is ever reported
    NO_FILE,                 ///< Terminal line names a path that was never written
    MULTICHIP,               ///< CSV carries per-chip columns instead of a summed psd_xyz
    ERROR,                   ///< Sweep dies after 3 lines with an adxl345 id mismatch
    KALICO,                  ///< Kalico dialect: two-part axis names, extra accel_per_hz column
};

/**
 * @brief Mock Moonraker client for testing without real printer connection
 *
 * Simulates printer hardware discovery with configurable test data.
 * Useful for UI development and testing without physical hardware.
 *
 * Inherits from MoonrakerClient to provide drop-in replacement compatibility.
 */
class MoonrakerClientMock : public helix::MoonrakerClient {
  public:
    using PrinterType = helix::mock::PrinterType;

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
     * @param factor Speed multiplier (clamped to the one range every
     *               simulated-time consumer shares, helix::sim::clamp_speed())
     */
    void set_simulation_speedup(double factor);

    /**
     * @brief Set the pause connect() takes before reporting CONNECTED
     *
     * Process-wide, in simulated milliseconds (scaled by sim_speed()). The
     * --test app keeps the default; the unit-test binary sets 0, because
     * connect() blocks its caller for the whole pause and no test waits on it.
     */
    static void set_connect_delay_ms(int ms);
    static int connect_delay_ms();

    /**
     * @brief Get current simulation speedup factor
     * @return Current speedup multiplier (1.0 = real-time)
     */
    double get_simulation_speedup() const;

    /**
     * @brief This instance's speedup as the shared simulated-time scale.
     *
     * Per-instance rather than helix::sim::SimSpeed::global(): two mocks in one
     * test process each carry their own factor, and set_simulation_speedup()
     * changes one of them mid-run.
     */
    [[nodiscard]] helix::sim::SimSpeed sim_speed() const;

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

    /// Klipper's toolhead.homed_axes as this mock currently reports it. Every
    /// status source (query, subscribe, notifications) reads it from here.
    std::string get_homed_axes() const {
        std::lock_guard<std::mutex> lock(homed_axes_mutex_);
        return homed_axes_;
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
    int connect(const char* url, helix::SharedCallback<void()> on_connected,
                helix::SharedCallback<void()> on_disconnected) override;

    /**
     * @brief Run the real discovery sequence under discovery_mutex_
     *
     * Every RPC the sequence sends is answered synchronously by the method
     * registry, so the whole sequence completes inside this call, rewriting
     * discovery_'s name lists that the simulation thread reads under the lock.
     * The callbacks run after the lock is released.
     */
    void discover_printer(
        helix::SharedCallback<void()> on_complete,
        helix::SharedCallback<void(const std::string& reason)> on_error = nullptr) override;

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
                                  helix::SharedCallback<void(const json&)> cb) override;

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
        const std::string& method, const json& params,
        helix::SharedCallback<void(const json&)> success_cb,
        helix::SharedCallback<void(const MoonrakerError&)> error_cb, uint32_t timeout_ms = 0,
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
     * @brief Kinematics the mock reports, in configfile and toolhead status
     * alike, so every consumer that asks sees the same machine.
     *
     * HELIX_MOCK_KINEMATICS overrides the printer type's default, so a test can
     * flip the persona without a new printer type. It is read once, at
     * construction: the simulation thread reports kinematics every tick, and a
     * getenv() there races a test's setenv() on the main thread.
     */
    std::string kinematics() const;

    /**
     * @brief Whether the mock should present a tool changer.
     *
     * Single source of truth for the two places that need the answer: this
     * client (which adds extruder1..3 and the toolchanger/tool objects) and
     * AmsBackend's mock branch (which picks the backend type). One function
     * stops the two env-var parses drifting apart.
     *
     * True when the effective mock AMS (helix::mock::effective_mock_ams: an
     * explicit HELIX_MOCK_AMS, else the persona's default) is a tool changer.
     * The creator5 persona defaults to one: a Creator 5 Pro is a 4-head
     * changer, so leaving it on the Happy Hare default misrepresents the
     * machine. An explicit HELIX_MOCK_AMS always wins, so other topologies stay
     * testable against the persona.
     *
     * Static because AmsBackend decides before any client instance exists.
     */
    static bool mock_toolchanger_selected();

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

    /**
     * @brief A chamber target arriving by the appliance's route around the
     *        gcode queue (or by gcode, for the same heater)
     *
     * With HELIX_MOCK_DRAGONBREATH_REJECT set, the appliance turns every target
     * down: the heater target stays 0 and the status object reports the
     * rejection. Otherwise the target is taken and any rejection cleared.
     * @return the route's reply, {"accepted", "reason", "seq"}
     */
    json chamber_route_set_target(double target);

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
     * @brief Dispatch printer status to notify AND method-callback registrants
     *
     * The live WebSocket path delivers notify_status_update to method-callback
     * registrants too (plugin subscriptions among them), so the mock's
     * synthetic status updates must reach those listeners as well.
     */
    void dispatch_status_update(const json& status, bool from_cached_snapshot = false,
                                bool whole_objects = false) override;

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
     * @brief Path the mock writes SHAPER_CALIBRATE data to, for @p axis_lower
     *
     * Per-process, because `make test-run` shards the suite across as many
     * helix-tests PROCESSES as the box has jobs, and three test files touch this
     * file: the panel integration and calibrator fixtures both std::remove() it
     * between cases, and the API test asserts on it. On a fixed path those land
     * in different shards and delete each other's fixture mid-run - the writer
     * then parses an absent CSV, freq_response comes back empty, and the verdict
     * row silently never populates. Embedding the PID makes concurrent shards
     * disjoint. Tests MUST use this rather than rebuilding the literal.
     */
    static std::string shaper_csv_path(char axis_lower);

    /**
     * @brief Delete both axes' CSVs for THIS process
     *
     * Fixtures call it on the way in (so a previous case in this process cannot
     * leak a stale file into the next) and on the way out (so a shard does not
     * strand its files in /tmp - each process now writes its own, where the old
     * fixed path was simply overwritten).
     */
    static void remove_shaper_csvs();

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

    /// Same, also setting [resonance_tester] hz_per_sec: how many Hz of sweep
    /// one simulated second covers, which sets the line cadence below.
    void set_resonance_sweep_range(double min_freq, double max_freq, double hz_per_sec) {
        resonance_min_freq_ = min_freq;
        resonance_max_freq_ = max_freq;
        resonance_hz_per_sec_ = hz_per_sec;
    }

    [[nodiscard]] double get_resonance_min_freq() const {
        return resonance_min_freq_;
    }

    [[nodiscard]] double get_resonance_max_freq() const {
        return resonance_max_freq_;
    }

    [[nodiscard]] double get_resonance_hz_per_sec() const {
        return resonance_hz_per_sec_;
    }

    /// Whether `[resonance_tester]` reports probe_points (at the bed centre,
    /// 20 mm up). Klipper requires them; off reproduces a fork that omits them.
    void set_resonance_probe_points(bool reported) {
        resonance_probe_points_ = reported;
    }

    /// `configfile.settings.resonance_tester` as the query and subscribe
    /// responses report it.
    [[nodiscard]] json resonance_tester_settings() const;

    /**
     * @name TEST_RESONANCES belt simulation
     * @{
     */

    /// Current simulated peak for a path ('A' or 'B').
    [[nodiscard]] float belt_peak_hz(char path) const;

    /// Seed both paths' simulated peaks. Re-testing a path walks its peak
    /// toward the other path's, so the defaults start with a visible delta.
    void set_belt_peaks_hz(float a_hz, float b_hz);

    /// Script one path's peak across its sweeps after the first, full or
    /// windowed; the last entry holds once the list runs out. A NaN entry
    /// writes a featureless curve (floor plus noise, no peak). While either
    /// path has a script, no sweep applies the re-measure walk.
    void set_belt_tune_script(char path_letter, std::vector<float> peaks_hz);

    /// Parse and apply a `<A|B>:<hz|flat>,...` script (HELIX_MOCK_BELT_TUNE_SCRIPT).
    /// Bad items are skipped with a warning; a bad path letter drops the spec.
    void apply_belt_tune_script(const std::string& spec);

    /// Switch the next TEST_RESONANCES to a failure mode (NONE = normal).
    void set_belt_failure(BeltMockFailure f) {
        belt_failure_ = f;
    }

    /// Milliseconds between "Testing frequency" lines; 0 = derive from
    /// hz_per_sec and sim speed. Tests set a small value.
    void set_belt_line_interval_ms(uint32_t ms) {
        belt_line_interval_ms_ = ms;
    }

    /// SHAPER_CALIBRATE plays whole-Hz sweep lines at the cadence
    /// TEST_RESONANCES uses (hz_per_sec and the sim speed) instead of the
    /// compact 20-line transcript, so the live preview has a sweep to follow.
    /// MoonrakerManager turns it on for the app; unit tests keep the compact one.
    void set_shaper_sweep_realtime(bool realtime) {
        shaper_sweep_realtime_ = realtime;
    }

    /// What the sweep this mock started last resonates like, matching the CSV
    /// it will write. Defaults until a sweep starts.
    [[nodiscard]] helix::calibration::MockSweepResponse active_sweep_response() const;

    /// The live preview's stream for a sweep of this mock: MockAccelBatchSource
    /// over active_sweep_response() at this mock's sim speed.
    [[nodiscard]] std::unique_ptr<helix::calibration::IAccelBatchSource>
    make_accel_batch_source(const helix::calibration::SweepPlan& plan) const;

    /**
     * @brief Path the next TEST_RESONANCES for @p name writes
     *
     * PID-scoped for the same reason as shaper_csv_path(): sharded test
     * processes must not delete each other's fixtures. Callers only ever use
     * the path from the console line, so it must match exactly.
     */
    [[nodiscard]] static std::string belt_csv_path(const std::string& axis_name,
                                                   const std::string& name);

    /// Delete every belt CSV this process wrote.
    static void remove_belt_csvs();

    /** @} */

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
     * @brief Override the extruder min_temp reported in configfile.settings
     *
     * Klipper accepts a negative min_temp - it is a sensor sanity floor, and a
     * toolchanger with no tool fitted needs one so the open thermistor read is
     * tolerated. Tests set it negative to exercise the adoption clamp (#1353).
     *
     * @param min_temp Extruder min_temp in celsius (default: 0.0)
     */
    void set_extruder_min_temp(double min_temp) {
        extruder_min_temp_ = min_temp;
    }

    /**
     * @brief Get the extruder min_temp reported in configfile.settings
     */
    [[nodiscard]] double get_extruder_min_temp() const {
        return extruder_min_temp_;
    }

    /**
     * @brief Override the extruder min_extrude_temp reported in configfile.settings
     *
     * @param min_extrude_temp Extruder min_extrude_temp in celsius (default: 170.0)
     */
    void set_extruder_min_extrude_temp(double min_extrude_temp) {
        extruder_min_extrude_temp_ = min_extrude_temp;
    }

    /**
     * @brief Get the extruder min_extrude_temp reported in configfile.settings
     */
    [[nodiscard]] double get_extruder_min_extrude_temp() const {
        return extruder_min_extrude_temp_;
    }

    /**
     * @brief Report an extra section in configfile.settings, replacing any the mock
     *        already builds under that name
     *
     * @param name     Section header, e.g. "extruder1"
     * @param settings Its parsed settings
     */
    void set_config_settings_section(const std::string& name, const json& settings) {
        extra_config_settings_[name] = settings;
    }

    /// Sections set_config_settings_section() added.
    [[nodiscard]] const json& extra_config_settings() const {
        return extra_config_settings_;
    }

    // --- HELIX_MOCK_SKIP_WRAPPERS: helix_skips.cfg is loaded (moonraker_client_mock_skips.cpp)

    /// The env knob is set.
    [[nodiscard]] static bool skip_wrappers_loaded();

    /// configfile sections the loaded file and the leveling objects it wraps
    /// contribute, as Klipper reports them in both settings and config.
    /// Empty when the file is not loaded.
    [[nodiscard]] json skip_wrapper_sections() const;

    /// Status of `_HELIX_PREP` and of the leveling objects' `applied`, keyed by
    /// object. Empty when the file is not loaded.
    [[nodiscard]] json skip_wrapper_status() const;

    /// HELIX_MOCK_LAYER_PAUSE (default on): the client.cfg layer-pause macros.
    [[nodiscard]] static bool layer_pause_macros_loaded();
    /// `gcode_macro SET_PRINT_STATS_INFO` with the schedule; empty when off.
    [[nodiscard]] json layer_pause_status() const;
    /// Add the three macros' objects to an objects list when loaded.
    static void append_layer_pause_objects(json& objects);
    /// The simulated print reached a new layer: fire a scheduled pause.
    void layer_pause_on_layer(int previous_layer, int current_layer);
    /// CANCEL_PRINT clears both schedules, as client.cfg's does.
    void reset_layer_pause();

    /// HELIX_MOCK_PLR with HELIX_MOCK_HELIX_PLUGIN: the plugin's `plr`
    /// feature entry for server.helix.status; null when the knob is off.
    /// `enabled` follows a written `helix-screen` / `plr_user`, on by default.
    [[nodiscard]] json mock_plr_feature_entry();
    /// A power-loss snapshot is on record: the newest history job reads
    /// `interrupted` until a discard or a resume clears it.
    [[nodiscard]] bool mock_plr_interrupted() const;
    /// The plugin's saving switch: a written `plr_user.enabled`, else on.
    [[nodiscard]] bool mock_plr_saving();
    /// Answer one server.helix.plr.* request (moonraker_client_mock_plr.cpp).
    void mock_plr_request(const std::string& method, const json& params,
                          helix::SharedCallback<void(const json&)> success_cb,
                          helix::SharedCallback<void(const MoonrakerError&)> error_cb);

    /// A print-start step reaching its wrapper: true, and the flag restored,
    /// when a skip was set for it.
    bool consume_skip(helix::skip_wrappers::Op op);

    /// Add the `gcode_macro` objects the loaded file defines to an objects
    /// list (a JSON array of names), from the leveling objects already in it.
    static void append_skip_wrapper_objects(json& objects);

    /**
     * @brief Report a status object from printer.objects.query, replacing any
     *        the mock already builds under that name
     *
     * @param name   Klipper object name, e.g. "machine_state_manager"
     * @param status Its status fields; served only when a query asks for the object
     */
    void set_object_status(const std::string& name, const json& status) {
        object_status_overrides_[name] = status;
    }

    /// Status objects set_object_status() added.
    [[nodiscard]] const json& object_status_overrides() const {
        return object_status_overrides_;
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
    /// Test seam: drive the Spoolman component in and out of server.info so the
    /// REAL discovery sequence's spoolman branch can be exercised both ways.
    void set_mock_spoolman_enabled(bool enabled) {
        mock_spoolman_enabled_ = enabled;
    }

    [[nodiscard]] bool is_mock_spoolman_enabled() const {
        return mock_spoolman_enabled_;
    }

    /// The Spoolman server behind server.spoolman.*, for tests to seed and inspect
    MockSpoolmanServer& spoolman_mock() {
        return spoolman_;
    }

    /// The webcams server.webcams.list answers with (HELIX_MOCK_WEBCAMS)
    [[nodiscard]] const std::vector<WebcamInfo>& mock_webcams() const {
        return mock_webcams_;
    }

    /// HELIX_MOCK_WEBCAMS: "Name[:service],Name[:service],..." -> the webcam
    /// list discovery publishes. Unset or empty -> empty (one unnamed feed).
    /// Entry N streams from /webcamN/ (the first from /webcam/); a service
    /// omitted is an MJPEG one.
    [[nodiscard]] static std::vector<WebcamInfo> parse_mock_webcams(const char* spec);

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

    /**
     * @brief Park or resume the simulation loop
     *
     * While parked the loop dispatches no status notifications and the tick
     * counter stands still, so simulated time does not advance either.
     *
     * Every push carries a full snapshot, and PrinterState applies any field
     * that differs from the subject's own value, so a push landing between a
     * test's `set` and its read undoes the write. Parking the loop is what
     * makes a value written by hand hold still long enough to measure.
     *
     * @param paused true to park the loop, false to resume it
     */
    void set_simulation_paused(bool paused) {
        {
            std::lock_guard<std::mutex> lock(sim_mutex_);
            simulation_paused_.store(paused);
        }
        sim_cv_.notify_all();
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

    /// The bed_mesh object as Klipper reports it: the active mesh and every stored
    /// profile with its points.
    [[nodiscard]] json bed_mesh_status() const;

    /**
     * @brief Check if bed mesh data is available
     * @return true if a valid bed mesh profile has been loaded
     */
    [[nodiscard]] bool has_bed_mesh() const {
        return !active_bed_mesh_.probed_matrix.empty();
    }

    /// Whether the printer reports motors-sync's `motors_sync` object (an AWD
    /// printer; HELIX_MOCK_OBJECTS="motors_sync" adds it to any persona). Its
    /// simulated print start then runs a motor sync after homing.
    [[nodiscard]] bool has_motors_sync() const;

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
     * @brief Append every non-chamber heater_generic (e.g. a filament dryer added
     *        through HELIX_MOCK_OBJECTS) to a synthesized frame, stepping each
     *        one's temperature @p dt simulated seconds toward its target.
     */
    void append_aux_heater_status(json& status_obj, double dt);

    /// Set a non-chamber heater_generic's target by its BARE name. False if unknown.
    bool set_aux_heater_target(const std::string& bare_name, double target);

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

    /// Whether a led_effect object (e.g. "led_effect rainbow") is running.
    /// Public because the object handlers are free-function lambdas taking a
    /// MoonrakerClientMock*, not members.
    bool led_effect_enabled(const std::string& object_name) const;

    /// An extruder's pressure advance K and smooth time, as SET_PRESSURE_ADVANCE
    /// last left them. An extruder never set reads the config default.
    struct PressureAdvance {
        double advance = 0.04;
        double smooth_time = 0.04;
    };
    PressureAdvance pressure_advance_for(const std::string& extruder) const;

    /// Current mock offset for a tool on one axis, in mm. Seeded distinct per
    /// tool AND per axis and updated by SET_TOOL_PARAMETER, so a value set
    /// earlier in the session survives into a later status snapshot instead of
    /// silently reverting. Public because the object handlers are
    /// free-function lambdas taking a MoonrakerClientMock*, not members.
    double tool_offset(int tool, helix::Axis axis) const;

    /// What printer.cfg holds for a tool before anything in the session moves
    /// it. Exposed so a test can assert a simulated write actually CHANGED the
    /// axis: a value that happens to equal the seed leaves it clean, and every
    /// assertion about saving that axis then passes without exercising it.
    static double tool_offset_seed(int tool, helix::Axis axis);

    /// klipper-toolchanger's parameter (and status field) name for an axis:
    /// gcode_x_offset / gcode_y_offset / gcode_z_offset.
    static const char* tool_offset_param(helix::Axis axis);

    /**
     * @brief Simulate CALIBRATE_TOOL_OFFSETS (klipper-toolchanger's example
     *        macro), when @p script is that command
     *
     * Blocking like the real macro: the rpc is answered only when the run is
     * over, and the run takes a few seconds of mock time on an lv_timer. On the
     * way it prints what the firmware prints - the toolchanger's "Selected tool
     * N (TN)" per tool, the probe's contact lines, "Sensor location at x,y,z"
     * for T0 and "Tool offset is x,y,z" for every other tool - and republishes
     * `toolchanger`.tool_number as the carriage changes tool. Each measured
     * tool's offsets are written exactly as the macro's _SAVE_TOOL_OFFSET does:
     * runtime value live and republished per axis, then staged for SAVE_CONFIG.
     *
     * HELIX_MOCK_TOOL_CAL_FAIL=<n> makes tool n's pass fail with the probe
     * tolerance error, so the failure path is reachable in --test.
     *
     * @return true if the script was handled here (the callbacks are owned)
     */
    bool
    simulate_tool_offset_calibration(const std::string& script,
                                     helix::SharedCallback<void(const nlohmann::json&)> success_cb,
                                     helix::SharedCallback<void(const MoonrakerError&)> error_cb);

    /**
     * @brief Simulate an automatic pressure-advance run
     *
     * Mirrors what a measuring firmware does on the console: a few candidate
     * probes a beat apart, then the applied value echoed the way Klipper's own
     * SET_PRESSURE_ADVANCE echoes it. Asynchronous, so the panel's phase list,
     * attempt counter and progress bar are all exercised in mock mode rather
     * than jumping straight to a result.
     *
     * @return true if the script was handled here
     */
    bool simulate_pa_calibration(const std::string& script,
                                 helix::SharedCallback<void(const nlohmann::json&)> success_cb,
                                 helix::SharedCallback<void(const MoonrakerError&)> error_cb);

    /**
     * @brief Simulate the Snapmaker U1's EXTRUDER_OFFSET_ACTION_* steps, when
     *        @p script is one of them
     *
     * Each step blocks like the firmware's: the rpc is answered a beat later on
     * an lv_timer. The firmware's state gate is modelled too: every step but
     * PRESTART refuses with its coded fault until PRESTART entered the
     * calibration state, and EXIT leaves it. A head's PROBE_CALIBRATE writes a
     * new extruder_offset, which the objects query then reports.
     *
     * HELIX_MOCK_NOZZLE_CAL_PLATE_REFUSE=<n> makes the first n plate checks
     * report the PEI sheet still on the bed; HELIX_MOCK_NOZZLE_CAL_FAIL=<n> makes
     * head n's probe fail.
     *
     * @return true if the script was handled here (the callbacks are owned)
     */
    bool simulate_nozzle_offset_calibration(
        const std::string& script, helix::SharedCallback<void(const nlohmann::json&)> success_cb,
        helix::SharedCallback<void(const MoonrakerError&)> error_cb);

    /**
     * @brief Answer an INDX command that blocks for minutes on the real
     *        printer (INDX_CALIBRATE, INDX_FAN_CALIBRATE, TC_CYCLE_ALL)
     *
     * The command runs at once; the rpc is answered later on an lv_timer,
     * scaled by the simulation speed, so a screen tracking it is observable.
     * A refusal answers at once.
     *
     * @return true if the script was handled here (the callbacks are owned)
     */
    bool simulate_indx_blocking(const std::string& script,
                                helix::SharedCallback<void(const nlohmann::json&)> success_cb,
                                helix::SharedCallback<void(const MoonrakerError&)> error_cb);

    /// A printer.gcode.script run line by line, as Klipper runs it.
    struct GcodeScriptRun {
        int result = 0;
        std::string error; ///< The first rejected line's message
        /// Where the script resumes once the MedusaHC swap a line started has
        /// ended; npos when every line ran.
        size_t resume_at = std::string::npos;
    };
    /// Run @p script from byte @p start, stopping after a line that starts a
    /// MedusaHC swap.
    GcodeScriptRun run_gcode_script_lines(const std::string& script, size_t start);
    /// Answer a printer.gcode.script rpc with what its lines did.
    void answer_gcode_script(const std::string& script, const GcodeScriptRun& run,
                             const helix::rpc_error_policy::CallerIntent& intent,
                             const helix::SharedCallback<void(const nlohmann::json&)>& success_cb,
                             const helix::SharedCallback<void(const MoonrakerError&)>& error_cb);
    /**
     * @brief Hold a printer.gcode.script reply until the MedusaHC swap its
     *        script started has ended.
     *
     * The controller's _wait_for_tool() blocks the command until the docks
     * confirm the tool or time out. A failure outside a print answers with the
     * controller's message and runs nothing after it; a success, or a failure
     * that paused a print, runs the rest of the script and then answers.
     */
    void hold_medusa_reply(std::string script, size_t resume_at,
                           helix::rpc_error_policy::CallerIntent intent,
                           helix::SharedCallback<void(const nlohmann::json&)> success_cb,
                           helix::SharedCallback<void(const MoonrakerError&)> error_cb);
    /// klipper-toolchanger's tool_number as pin_watch's sync_toolchanger keeps
    /// it: the docks' tool when they name one, -1 (UNSELECT_TOOL) when they do
    /// not, and while printing an unnamed reading leaves it alone.
    int sync_medusa_toolchanger_tool();
    /// Tools the MedusaHC variant carries: six on medusahc-tn, four otherwise.
    [[nodiscard]] static int medusa_tool_count();
    static constexpr int kMedusaMaxTools = 6;
    /// MHC_RESUME refuses to re-pick a tool whose nozzle is at or below this.
    static constexpr int kMedusaResumeMinNozzleC = 190;

    /// The U1 persona's extruder_offset for head @p head (0-3), as the
    /// objects query reports it.
    [[nodiscard]] std::array<double, 3> nozzle_offset(int head) const;

    /// Whether the simulated firmware holds the offset calibration state
    /// (PRESTART ran, EXIT has not). machine_state_manager.main_state reports it.
    [[nodiscard]] bool nozzle_calibration_active() const;

    /// The U1 persona's fitted nozzle on head @p head (0-3): its diameter in mm
    /// and flow type, as the extruder object reports them.
    [[nodiscard]] std::pair<double, std::string> nozzle_properties(int head) const;

    /// The U1 firmware's control/nozzle_properties webhook: @p params carries
    /// extruder and optionally diameter and volume_type. Refuses the way the
    /// firmware does, inside the reply ({"state": "error", "message": ...}),
    /// for no extruder, an unknown one, a print printing or paused, or a value
    /// it does not accept; otherwise stores the change, pushes the extruder's
    /// status update and answers {"state": "success"}.
    nlohmann::json set_nozzle_properties(const nlohmann::json& params);

    /// The inverse: which axis a PARAMETER= names, or nullopt for any other
    /// tool parameter (the mock models only the three offsets).
    static std::optional<helix::Axis> tool_offset_axis(const std::string& param);

    /// Klipper's configfile.save_config_pending - whether a SAVE_CONFIG is owed.
    /// Set by anything routed through configfile.set() (here:
    /// SAVE_TOOL_PARAMETER), cleared when SAVE_CONFIG commits.
    bool save_config_pending() const;

    /// Klipper's configfile.save_config_pending_items, {section: {option:
    /// value}} with STRING values - configfile.set() stores str(value).
    nlohmann::json save_config_pending_items() const;

    /**
     * @brief Simulate a Klipper restart - the ONE model of what a restart does
     *
     * Sets klippy_state to STARTUP, clears the active print, zeroes heater
     * targets, drops excluded objects, reloads everything Klipper would re-read
     * from printer.cfg (per-tool z-offsets), dispatches the webhooks status
     * update a real restart arrives as, then returns to READY after a delay on a
     * tracked thread (not an lv_timer - this must work in tests that never pump
     * LVGL).
     * Temps continue cooling naturally during the restart period.
     *
     * Public because the printer.restart / printer.firmware_restart handlers are
     * free-function lambdas taking a MoonrakerClientMock*, not members. They
     * delegate here rather than each faking a restart differently.
     *
     * @param is_firmware true for FIRMWARE_RESTART (3s), false for RESTART (2s),
     *                    both divided by the speedup factor
     */
    void trigger_restart(bool is_firmware);

    /**
     * @brief `[medusahc]` status in the schema of the selected variant.
     *
     * Empty object when this is not a MedusaHC mock. Built
     * from the swap-simulation atomics, so a caller polling this while a swap
     * runs sees the phases advance. Safe to call from either thread.
     */
    nlohmann::json medusa_status_json() const;
    /// HELIX_MOCK_AMS=openams, and the `oams_manager` frame it serves (public:
    /// the printer.objects.query / subscribe handlers read it).
    bool is_mock_openams() const;
    [[nodiscard]] nlohmann::json openams_status_json() const;
    /// HELIX_MOCK_OPENAMS_UNITS=shared: an AMS HT and an AMS 2 Pro on one lane.
    [[nodiscard]] static bool openams_shared_lane_units();
    /// HELIX_MOCK_OPENAMS_UNITS=two_lanes: that pair on lane `fps`, two AMS 2 Pro on `fps2`.
    [[nodiscard]] static bool openams_two_lane_units();
    /// HELIX_MOCK_OPENAMS_UNITS=fleet: twelve units (an AMS HT, eleven AMS 2 Pro), 45 bays,
    /// on two FPS lanes.
    [[nodiscard]] static bool openams_fleet_lane_units();
    /// Any of the three shapes: the units the openams plugin describes, one filament group per
    /// slot.
    [[nodiscard]] static bool openams_plugin_units();
    /// Units the plugin shape publishes (0 when no shape is selected).
    [[nodiscard]] static int openams_unit_count();
    /// Slots across those units.
    [[nodiscard]] static int openams_slot_count();

    /**
     * @brief The `zmod_color` object as Z-Mod's firmware reports it.
     *
     * Full frame: active_tool_id, total_tools, color_limit,
     * display, valid_types, hidden_types, palette (the 24 firmware colours)
     * and slots (the 4 heads' Material/HEX registry). Served by the
     * objects.query/subscribe handlers and republished on _T_IN/_T_OUT/
     * CHANGE_ZCOLOR.
     */
    json zmod_color_status() const;

    /**
     * @brief `[pin_watch io]` status: `{"current_tool": int}`.
     *
     * Empty object for NONE.
     */
    nlohmann::json pin_watch_status_json() const;
    /// The macro state the MedusaHC controller keeps beside [medusahc]:
    /// `gcode_macro _TOOL_OFFSET`, `gcode_macro _GLOBAL_STATE` and
    /// `save_variables`, keyed by object name. Empty when not a MedusaHC mock.
    nlohmann::json medusa_macro_status_json() const;
    /// The nozzle temperature of hot end @p tool: the live extruder for tool 0,
    /// secondary_extruder_temp() for the rest.
    [[nodiscard]] double medusa_nozzle_temp(int tool) const;
    /// Secondary extruder @p index (1..5)'s reading and target. They start
    /// apart (extruder1 heating, extruder2 at temperature, extruder3 cooling,
    /// the rest cold) so each heating state shows, and follow their targets
    /// like the primary: SET_HEATER_TEMPERATURE HEATER=extruder<n> sets one.
    [[nodiscard]] double secondary_extruder_temp(int index) const;
    [[nodiscard]] double secondary_extruder_target(int index) const;

    /**
     * @brief U1 `defect_detection` status: the module's stored config, which
     *        is what its get_status() returns.
     *
     * Empty object for every persona but the U1. main_enable and
     * clean_bed.enable follow DEFECT_DETECTION_CONFIG MAIN_ENABLE=0|1 and
     * CLEAN_BED_ENABLE=0|1.
     */
    nlohmann::json defect_detection_status() const;

    /**
     * @brief U1 `purifier` status: the top module's air purifier, whose
     *        power_detected is the top being fitted.
     *
     * Empty object for every persona but the U1. power_detected is true unless
     * HELIX_MOCK_ENCLOSURE_TOP=0.
     */
    nlohmann::json purifier_status() const;

  private:
    // Test visibility into the chamber-key cache and the synchronous initial
    // state dispatch (see tests/test_helpers/moonraker_client_mock_test_access.h).
    friend class helix::MoonrakerClientMockTestAccess;

    /// One console line the pressure-advance simulation still owes, and when.
    struct PendingPaLine {
        std::chrono::steady_clock::time_point due;
        std::string line;
        bool is_final;
        helix::SharedCallback<void(const nlohmann::json&)> success_cb;
        helix::SharedCallback<void(const MoonrakerError&)> error_cb;
    };
    /// HELIX_MOCK_PA_FAIL=1 makes the run refuse with a realistic runout
    /// message. The refusal path is otherwise unreachable in mock mode, and it
    /// is one of the states the panel handles most visibly.
    std::vector<PendingPaLine> pending_pa_lines_;
    mutable std::mutex pa_cal_mutex_;

    /// U1 offset calibration state: whether PRESTART entered the firmware's
    /// calibration state, how many plate checks are still to refuse, and each
    /// head's extruder_offset. Under nozzle_cal_mutex_.
    bool nozzle_cal_active_ = false;
    int nozzle_cal_plate_refusals_ = 0;
    std::array<std::array<double, 3>, 4> nozzle_offsets_{{{0.073, -0.037, 0.0},
                                                          {0.089, 0.037, 0.099},
                                                          {0.012, 0.019, 0.272},
                                                          {-0.089, -0.016, 0.060}}};
    /// Each head's fitted nozzle (the stock U1: 0.4 mm standard on every head),
    /// also under nozzle_cal_mutex_.
    std::array<std::pair<double, std::string>, 4> nozzle_props_{
        {{0.4, "standard"}, {0.4, "standard"}, {0.4, "standard"}, {0.4, "standard"}}};
    mutable std::mutex nozzle_cal_mutex_;
    void service_pending_pa_lines();

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
     * @brief Detect toolchanger mock mode
     *
     * Returns mock_toolchanger_selected(): true when the mode
     * helix::mock::effective_mock_ams() resolves (an explicit HELIX_MOCK_AMS,
     * else the persona's default) is "toolchanger", "tool_changer" or "tc". Used to gate
     * the multi-extruder toolchanger emulation (4 distinct extruder heaters,
     * toolchanger + tool objects) so single-extruder/AFC/MMU/mixed/IFS modes are
     * unaffected.
     */
    bool is_mock_toolchanger() const;

  public:
    /**
     * @brief Which MedusaHC shipping configuration the mock emulates.
     *
     * Irbis3D's Python controller runs with or without klipper-toolchanger (see
     * docs/devel/FILAMENT_BACKEND_MEDUSAHC.md). Each variant emits the object set
     * its configuration has, so toolchanger_addon's detection and swap command
     * routing run outside the unit tests too.
     */
    using MedusaVariant = helix::mock::MedusaVariant;

    /**
     * @brief MedusaHC variant selected by HELIX_MOCK_AMS, or NONE.
     *
     * "medusahc"/"medusa"/"mhc" select CONTROLLER, "medusahc-ktc" CONTROLLER_KTC and
     * "medusahc-tn" CONTROLLER_TN
     * (helix::mock::medusa_variant). Unlike
     * is_mock_toolchanger(), these values do NOT build an AmsBackendMock:
     * try_create_mock() declines them so real discovery runs and the production
     * AmsBackendToolChanger drives the mock objects.
     */
    static MedusaVariant mock_medusa_variant();

    /**
     * @brief Someone puts the hot ends back by hand: @p tool seated on the
     * carriage (-1 for none) and every other one in its dock.
     *
     * The dock switches then agree, so pin_watch's current_tool is definite
     * again. The controller's sensor_error latch is NOT touched: only its next
     * operation clears it.
     */
    void medusa_place_tools_by_hand(int tool);

    /// Convenience: mock_medusa_variant() != MedusaVariant::NONE.
    bool is_mock_medusahc() const;

    /// configfile sections a MedusaHC persona adds, as Klipper reports them in
    /// both settings and config: the upstream example RESUME, which runs
    /// MHC_RESUME. Empty for any other persona.
    [[nodiscard]] json medusa_config_sections() const;

  private:
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

    /// Probe a fresh mesh, store it as @p profile_name, and publish bed_mesh.
    void calibrate_mock_mesh(const std::string& profile_name);

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

    /// True when the chamber reading is simulated: the default chamber sensor,
    /// or the K2 Plus's "temperature_sensor chamber_temp".
    bool simulates_chamber_temp() const;

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
    /// Republish one tool's offset on one axis after SET_TOOL_PARAMETER.
    void dispatch_tool_update(int tool, helix::Axis axis);

    /// What the macro's _SAVE_TOOL_OFFSET does for one tool: SET_TOOL_PARAMETER
    /// on each axis (live + republished) then SAVE_TOOL_PARAMETER (staged).
    void apply_calibrated_tool_offset(int tool, double x, double y, double z);

    /// Republish `toolchanger`.tool_number, as a tool change does.
    void dispatch_toolchanger_tool(int tool);

    /// Klipper's configfile.set(): stage one option for the next SAVE_CONFIG.
    /// Does NOT change any runtime value - on a real printer the runtime write
    /// already happened (SET_TOOL_PARAMETER) and this only marks the config
    /// dirty.
    void stage_config_change(const std::string& section, const std::string& option,
                             const std::string& value);

    /// Klipper's cmd_SAVE_CONFIG: fold the staged options into the durable
    /// stores and clear the pending set. The restart is the caller's, matching
    /// Klipper, where writing the file and restarting are separate steps.
    void commit_pending_config();

    /// Republish configfile.save_config_pending{,_items}.
    void dispatch_configfile_update();

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
     * @brief Play console lines one per tick, then run on_done
     *
     * The timer and its payload are tracked in calibration_timers_, so a mock
     * torn down mid-playback frees both and on_done never runs.
     */
    void play_console_lines(std::vector<std::string> lines, uint32_t interval_ms,
                            helix::SharedCallback<void()> on_done);

    /// play_console_lines() for ticks that do more than print one line: each
    /// step runs on its own tick, then on_done on the tick after the last.
    void play_console_steps(std::vector<helix::SharedCallback<void()>> steps, uint32_t interval_ms,
                            helix::SharedCallback<void()> on_done);

    /// Milliseconds between whole-Hz "Testing frequency" lines: the
    /// set_belt_line_interval_ms() override, else one simulated second per
    /// hz_per_sec Hz shortened by the sim speed. Shared by both sweeps.
    [[nodiscard]] uint32_t sweep_line_interval_ms() const;

    /**
     * @brief Dispatch TEST_RESONANCES response sequence
     *
     * Simulates Klipper's TEST_RESONANCES AXIS=<1,1|1,-1> OUTPUT=resonances
     * NAME=<name>: one "Testing frequency" line per whole Hz, then the CSV
     * write line the belt-tension collector terminates on. Failure modes
     * (set_belt_failure) reproduce the ways a real run goes wrong.
     */
    void dispatch_test_resonances_response(const std::string& gcode);

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

    /// Test helper: the next send of @p method answers @p err through its error
    /// callback without reaching the handler, so nothing the handler would
    /// change has changed. One-shot. Works for any method in the registry.
    void fail_next(const std::string& method,
                   MoonrakerError err = MoonrakerError::unknown("mock: injected failure"));

    /// Test helper: the next send of @p method is held unanswered until
    /// fire_deferred() runs it through its handler or fire_deferred_error()
    /// fails it. Until then the handler has not run. One-shot; firing with
    /// nothing held is a no-op.
    void defer_next(const std::string& method);
    void fire_deferred(const std::string& method);
    void fire_deferred_error(const std::string& method, const MoonrakerError& err);

    /// Test inspection: sends of @p method so far, failed and deferred included.
    [[nodiscard]] int call_count(const std::string& method) const;

    /// What machine.update.status reports as `busy` (an update_manager job running).
    void set_update_manager_busy(bool busy) {
        update_manager_busy_ = busy;
    }
    [[nodiscard]] bool update_manager_busy() const {
        return update_manager_busy_;
    }

    /// Test helper: what server.helix.analysis answers for @p filename (its
    /// `result`); a null @p result forgets it, and such a file answers pending.
    void set_mock_file_analysis(const std::string& filename, const json& result) {
        if (result.is_null()) {
            mock_file_analysis_.erase(filename);
        } else {
            mock_file_analysis_[filename] = result;
        }
    }
    [[nodiscard]] json mock_file_analysis(const std::string& filename) const {
        const auto it = mock_file_analysis_.find(filename);
        return it == mock_file_analysis_.end() ? json(nullptr) : it->second;
    }

    /// Test helper: the Moonraker database the server.database.* handlers serve.
    /// Keys are dotted the way Moonraker's are ("a.b" is member b of record a).
    void mock_db_set(const std::string& ns, const std::string& key, const json& value);
    /// The value at a dotted key, or null when absent.
    [[nodiscard]] json mock_db_get(const std::string& ns, const std::string& key);
    /// Every namespace, for the handlers.
    json& mock_db() {
        return mock_db_;
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

    /// Test helper: when the next printer.gcode.script containing @p script_substr
    /// runs, emit @p line as a gcode response before its RPC answers, the way
    /// Klipper declines a command on the console and still answers `ok`. One-shot.
    void force_next_gcode_console_reply(const std::string& script_substr, const std::string& line) {
        std::lock_guard<std::mutex> lock(forced_gcode_error_mutex_);
        forced_console_reply_ = std::make_pair(script_substr, line);
    }

    /// Test helper: the next gcode line containing @p script_substr probes a mesh and
    /// stores it in @p profile whatever PROFILE= it names, the way G29 or a wrapper
    /// macro that drops its parameters stores in default. One-shot.
    void force_next_mesh_calibration(const std::string& script_substr, const std::string& profile) {
        std::lock_guard<std::mutex> lock(forced_gcode_error_mutex_);
        forced_mesh_calibration_ = std::make_pair(script_substr, profile);
    }

    /// Consume a pending forced mesh calibration if @p line matches; the profile it stores in.
    std::optional<std::string> take_forced_mesh_calibration(const std::string& line) {
        std::lock_guard<std::mutex> lock(forced_gcode_error_mutex_);
        if (!forced_mesh_calibration_ ||
            line.find(forced_mesh_calibration_->first) == std::string::npos)
            return std::nullopt;
        std::string profile = forced_mesh_calibration_->second;
        forced_mesh_calibration_.reset();
        return profile;
    }

    /// Consume a pending forced console reply if it matches @p script.
    std::optional<std::string> take_forced_console_reply(const std::string& script) {
        std::lock_guard<std::mutex> lock(forced_gcode_error_mutex_);
        if (!forced_console_reply_ ||
            script.find(forced_console_reply_->first) == std::string::npos)
            return std::nullopt;
        std::string line = forced_console_reply_->second;
        forced_console_reply_.reset();
        return line;
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
    std::string kinematics_override_; ///< HELIX_MOCK_KINEMATICS at construction

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

    /// True only between an explicit disconnect() and the next successful
    /// connect(): during that window send_jsonrpc refuses with CONNECTION_LOST
    /// like the real client. Construction leaves it false so fixtures that
    /// never simulate a link keep getting answers (the mock's initial
    /// ConnectionState is DISCONNECTED and must not gate sends by itself).
    bool sim_link_down_{false};
    bool update_manager_busy_{false};

    // Intent of the in-flight send_jsonrpc() dispatch, read by the method
    // handlers via current_send_intent().
    helix::rpc_error_policy::CallerIntent current_send_intent_{};

    // Generic fault injection (fail_next / defer_next) and per-method send
    // counts. Guarded by fault_mutex_; callbacks never run under it.
    struct HeldRequest {
        json params;
        helix::SharedCallback<void(const json&)> success_cb;
        helix::SharedCallback<void(const MoonrakerError&)> error_cb;
    };
    std::map<std::string, MoonrakerError> fail_next_;
    std::set<std::string> defer_next_;
    std::map<std::string, HeldRequest> held_requests_;
    std::map<std::string, int> call_counts_;
    mutable std::mutex fault_mutex_;

    // Unlocked: the handlers answer on the calling thread, and every caller of
    // the database and Spoolman routes is on the main thread.
    json mock_db_ = json::object();
    std::map<std::string, json> mock_file_analysis_;
    MockSpoolmanServer spoolman_;

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
    // One-shot console reply for a matching printer.gcode.script: {script substring,
    // line}. Shares the mutex below.
    std::optional<std::pair<std::string, std::string>> forced_console_reply_;
    // One-shot forced mesh calibration: {line substring, profile it stores in}.
    std::optional<std::pair<std::string, std::string>> forced_mesh_calibration_;
    mutable std::mutex forced_gcode_error_mutex_;

    // Temperature simulation state
    std::atomic<double> extruder_temp_{25.0};  // Current temperature
    std::atomic<double> extruder_target_{0.0}; // Target temperature (0 = off)
    std::atomic<double> bed_temp_{25.0};       // Current temperature
    std::atomic<double> bed_target_{0.0};      // Target temperature (0 = off)
    std::atomic<double> chamber_temp_{25.0};   // Chamber temp (25-45°C, passive sensor)
    std::atomic<double> chamber_target_{0.0};  // Chamber target temperature (0 = off)
    // Non-chamber heater_generic objects, keyed by full Klipper name: {temp, target}
    std::map<std::string, std::pair<double, double>> aux_heaters_;
    std::mutex aux_heaters_mutex_;
    std::atomic<double> mcu_temp_{42.0};  // MCU temp (40-55°C, stable with small variation)
    std::atomic<double> host_temp_{52.0}; // Host/RPi temp (45-65°C, correlates with load)

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
    // Mode saved by the last SAVE_GCODE_STATE, for the matching
    // RESTORE_GCODE_STATE. One deep: the CFS load envelope and the K1 chute
    // jog script never nest saves.
    std::atomic<bool> saved_gcode_relative_{false};
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
    /// Last layer announced on the console. Sim thread only.
    int last_console_layer_ = 0;
    int layer_pause_last_layer_ = 0; ///< status thread only
    /// server.helix.plr.* state: the snapshot is kept until a discard or a
    /// committed resume, and each plan request issues a new plan id. Main
    /// thread only, like every mock RPC handler.
    bool plr_snapshot_cleared_ = false;
    int plr_plan_seq_ = 0;
    std::string plr_plan_id_;
    /// server.helix.plr.mesh_include state: printer.cfg's include line, as
    /// the add/remove actions leave it.
    bool plr_mesh_include_ = false;
    std::atomic<bool> layer_pause_next_{false};
    std::atomic<int> layer_pause_next_armed_at_{0}; ///< layer the request arrived on
    std::atomic<bool> layer_pause_at_enable_{false};
    std::atomic<int> layer_pause_at_layer_{0};
    std::atomic<int> speed_factor_{100}; // Percentage
    std::atomic<int> flow_factor_{100};  // Percentage
    std::atomic<int> fan_speed_{0};      // 0-255

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
    /// Guarded by pressure_advance_mutex_.
    std::map<std::string, PressureAdvance> pressure_advance_;
    mutable std::mutex pressure_advance_mutex_;

    std::map<std::string, LedColor> led_states_; // LED name -> color
    mutable std::mutex led_mutex_;               // Protects led_states_
    /// Klipper object names of the enabled LED effects. SET_LED_EFFECT is
    /// exclusive (enabling one makes the set exactly {that one}); STOP=1
    /// removes one; STOP_LED_EFFECTS clears it. Guarded by led_mutex_.
    std::set<std::string> enabled_led_effects_;

    // Klippy service state (for RESTART/FIRMWARE_RESTART simulation)
    std::atomic<KlippyState> klippy_state_{KlippyState::READY};

    // Fan speed tracking (multiple fans by name)
    std::map<std::string, double> fan_speeds_; // Fan name -> speed (0.0-1.0)
    mutable std::mutex fan_mutex_;             // Protects fan_speeds_

    // G-code offset tracking
    std::atomic<double> gcode_offset_z_{0.0}; // Z offset from SET_GCODE_OFFSET
    std::atomic<double> gcode_offset_x_{0.0}; // X offset (MHC_TOOL_OFFSET applies X/Y/Z)
    std::atomic<double> gcode_offset_y_{0.0};
    /// Per-tool offsets, indexed by tool number then axis, driven by
    /// SET_TOOL_PARAMETER. Seeded DISTINCT rather than all-zero, per tool and
    /// per axis: an all-zero seed makes "every tool reads the same value" (or
    /// "X shows Z's number") — the exact bugs a per-tool display can have —
    /// look correct.
    mutable std::mutex tool_offsets_mutex_;
    std::map<int, std::map<helix::Axis, double>> tool_offsets_;
    /// The durable copy - what printer.cfg holds, i.e. what the tool comes back
    /// with after a restart. SAVE_CONFIG commits the staged values into here.
    ///
    /// Keeping this SEPARATE from the runtime map is the whole point: without
    /// it an offset that was set and never saved survived a restart too, so the
    /// mock could not tell a persisted save from a forgotten one and no test of
    /// the persist path could fail.
    std::map<int, std::map<helix::Axis, double>> tool_offsets_saved_;

    /// Klipper's configfile.save_config_pending_items - {section: {option:
    /// value}}, values stringified as configfile.set() does.
    mutable std::mutex pending_config_mutex_;
    std::map<std::string, std::map<std::string, std::string>> pending_config_items_;

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
        MOTOR_SYNC = 3,         // "Motors synchronization started", with motors_sync only
        HEATING_BED = 4,        // "M190 S60" dispatched
        HEATING_NOZZLE = 5,     // "M109 S210" dispatched
        QGL = 6,                // "QUAD_GANTRY_LEVEL" dispatched
        BED_MESH = 7,           // "BED_MESH_CALIBRATE" dispatched
        PURGING = 8,            // "VORON_PURGE" dispatched
        LAYER_1 = 9             // "SET_PRINT_STATS_INFO CURRENT_LAYER=1" dispatched
    };
    std::atomic<uint8_t> simulated_print_start_phase_{0};

    // Simulation thread control
    std::thread simulation_thread_;
    std::atomic<bool> simulation_running_{false};
    std::mutex sim_mutex_;           // For condition variable wait
    std::condition_variable sim_cv_; // For interruptible sleep during shutdown
    // set_simulation_paused(): the loop parks on this, dispatching nothing
    std::atomic<bool> simulation_paused_{false};

    // Protects the discovery_ name lists (heaters/fans/sensors/leds/filament_sensors)
    // against the temperature simulation thread.
    //
    // connect() starts that thread, and discovery then REASSIGNS those
    // vectors on the calling thread — `discovery_.fans() = {...}` frees every old
    // std::string buffer while the simulation loop is iterating them. ASan caught
    // this as two heap-use-after-frees (memcpy and memcmp on freed string data)
    // from FullStackTestFixture, whose constructor does exactly that sequence.
    //
    // Lock discipline, verified against the call graph — do not nest these:
    //   LOCKED   discover_printer() (the whole synchronous real sequence),
    //            populate_hardware(), populate_capabilities(),
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
    static constexpr double kRoomTemp = 25.0;
    static constexpr double kExtruderHeatRate = 3.0; // °C/sec when heating
    static constexpr double kExtruderCoolRate = 1.5; // °C/sec when cooling
    /// One simulation step of @p dt simulated seconds for every secondary
    /// extruder: toward its target, or toward room temperature when off.
    void advance_secondary_extruders(double dt);
    struct SecondaryExtruder {
        double temperature = kRoomTemp;
        double target = 0.0;
    };
    /// Index 0 is the primary (extruder_temp_), unused here. Guarded by
    /// secondary_extruders_mutex_.
    std::array<SecondaryExtruder, 6> secondary_extruders_{{{kRoomTemp, 0.0},
                                                           {150.0, 250.0},
                                                           {248.0, 250.0},
                                                           {200.0, 0.0},
                                                           {kRoomTemp, 0.0},
                                                           {kRoomTemp, 0.0}}};
    mutable std::mutex secondary_extruders_mutex_;
    static constexpr double kBedHeatRate = 1.0;          // °C/sec when heating
    static constexpr double kBedCoolRate = 0.3;          // °C/sec when cooling
    static constexpr double kChamberHeatRate = 0.3;      // °C/sec when heating (slower than bed)
    static constexpr double kChamberCoolRate = 0.1;      // °C/sec when cooling
    static constexpr int kSimulationIntervalMs = 250;    // Physics tick interval
    static constexpr int kNotificationIntervalTicks = 4; // Dispatch every 4 ticks (~1s)

    // Mock service availability flags (initialized from env vars in constructor)
    bool mock_spoolman_enabled_{true};     ///< Controlled by HELIX_MOCK_SPOOLMAN env var
    std::vector<WebcamInfo> mock_webcams_; ///< Published at discovery; HELIX_MOCK_WEBCAMS

  public:
    /// Entries added through server.webcams.post_item, listed after the HELIX_MOCK_WEBCAMS
    /// ones. Keyed by uid, or by name, like Moonraker's own registry.
    struct PostedWebcams {
        std::mutex mutex;
        std::vector<nlohmann::json> items;
        int next_uid = 0;
    };
    PostedWebcams& posted_webcams() {
        return posted_webcams_;
    }

  private:
    PostedWebcams posted_webcams_;
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
    double extruder_min_temp_{0.0};      ///< Extruder min_temp reported in configfile.settings
    double extruder_min_extrude_temp_{170.0}; ///< Extruder min_extrude_temp in configfile.settings
    json extra_config_settings_ = json::object();   ///< set_config_settings_section() sections
    std::atomic<int> skip_flags_[3] = {1, 1, 1};    ///< `_HELIX_PREP` run_* by skip_wrappers::Op
    json object_status_overrides_ = json::object(); ///< set_object_status() statuses
    double resonance_min_freq_{5.0};    ///< [resonance_tester] min_freq the mock reports/sweeps
    double resonance_max_freq_{135.0};  ///< [resonance_tester] max_freq the mock reports/sweeps
    double resonance_hz_per_sec_{1.0};  ///< [resonance_tester] hz_per_sec the mock reports/sweeps
    bool resonance_probe_points_{true}; ///< [resonance_tester] reports probe_points
    /// Simulated belt peaks, index 0 = path A ('1,1'), 1 = path B ('1,-1').
    float belt_peaks_hz_[2]{110.0f, 98.0f};
    /// Measurements taken per path; every one after the first walks that
    /// path's peak toward the other's, so repeated tuning converges.
    int belt_measure_count_[2]{0, 0};
    /// Per-path scripted peaks for every sweep after the first (NaN = flat)
    /// and the next entry to play.
    std::vector<float> belt_script_[2];
    size_t belt_script_pos_[2]{0, 0};
    BeltMockFailure belt_failure_{BeltMockFailure::NONE};
    /// 0 = derive the line interval from hz_per_sec and the sim speed.
    uint32_t belt_line_interval_ms_{0};
    bool shaper_sweep_realtime_{false};
    /// Set where a sweep is dispatched, read where the preview attaches.
    mutable std::mutex sweep_response_mutex_;
    helix::calibration::MockSweepResponse sweep_response_;

    bool mmu_enabled_{true}; ///< MMU available (default true for existing tests)

    // --- MedusaHC swap simulation -------------------------------------------
    // Driven from gcode_script() (the swap, unmount and feeder commands) and
    // advanced by temperature_simulation_loop(), so an operation moves through
    // the phases the real controller reports instead of snapping to its end.
    // Written from the caller's thread and the simulation thread, read by the
    // RPC handlers on any thread: every field sits behind medusa_mutex_.
    enum class MedusaPhase { Idle, Changing, Dropping, Picking };
    struct MedusaSim {
        int carriage_tool = 0; ///< Hot end physically on the carriage, -1 none
        /// A dock whose switch reads seated though its hot end is not in it,
        /// -1 none. pin_watch then cannot tell which tool is mounted (-2).
        int stuck_dock = -1;
        /// [medusahc].target_tool: the last operation's tool, kept after it ends.
        int target_tool = -1;
        int pick_tool = -1;            ///< What the running operation picks up, -1 none
        bool close_after_drop = false; ///< DROP_TOOL / UNSELECT_TOOL re-grip after parking
        MedusaPhase phase = MedusaPhase::Idle;
        int phase_ticks = 0; ///< Notifications left in the current phase
        bool feeder_open = false;
        /// [medusahc].sensor_error and last_error: set when an operation fails,
        /// cleared only when the next one begins.
        bool op_failed = false;
        std::string last_error;
        int redock_ticks = -1; ///< Notifications until the simulated hand re-dock, -1 none
        /// Set when a command arms a phase: the script that sent it stops there.
        bool swap_armed = false;
        /// `toolchanger`.tool_number as pin_watch last synced it, and the
        /// pin reading it synced from.
        int toolchanger_tool = 0;
        int toolchanger_reading = 0;
        bool toolchanger_synced = false;
        /// [medusahc].layer, from MHC_LAYER_SET.
        int layer = 0;
        /// `_GLOBAL_STATE`: the tool RESUME re-picks (PAUSE saves the mounted
        /// one, SET and DROP their own), and the failed-change flag MHC_ERROR
        /// sets while a print is active.
        int global_target_tool = -1;
        int error_state = 0;
        /// The swap MHC_RESUME started: the print continues once it succeeds.
        bool resume_after_swap = false;
        /// `_TOOL_OFFSET` t<n>_off_{x,y,z}, by tool then axis.
        std::array<std::array<double, 3>, kMedusaMaxTools> offsets{};
        /// save_variables' variables (t<n>_gcode_<axis>_offset and anything else saved).
        nlohmann::json saved_variables = nlohmann::json::object();
    };
    /// A printer.gcode.script reply held until the swap its script started ends.
    struct MedusaReply {
        std::string script;
        size_t resume_at = 0;
        helix::rpc_error_policy::CallerIntent intent{};
        helix::SharedCallback<void(const nlohmann::json&)> success_cb;
        helix::SharedCallback<void(const MoonrakerError&)> error_cb;
    };
    mutable std::mutex medusa_mutex_;
    MedusaSim medusa_;
    std::optional<MedusaReply> medusa_reply_; ///< Behind medusa_mutex_
    /// Held replies are answered on the main thread; a mock destroyed first
    /// drops them.
    helix::AsyncLifetimeGuard medusa_reply_lifetime_;
    int medusa_swap_fail_tool_ = -1;       ///< HELIX_MOCK_MEDUSA_SWAP_FAIL, set in the ctor
    int medusa_redock_notifications_ = -1; ///< HELIX_MOCK_MEDUSA_REDOCK_S, set in the ctor

    // --- Z-Mod (Creator 5 Pro) mock hardware ----------------------------------
    // Same shape as the MedusaHC block above: gcode_script() writes on the
    // caller's thread, the RPC object handlers read from any thread, so the
    // tool id is atomic and the slot registry sits behind its own mutex.
    /// zmod_color.active_tool_id: 0..3 mounted, -1 nothing on the carriage.
    std::atomic<int> zmod_active_tool_{-1};
    mutable std::mutex zmod_mutex_;
    /// (Material, HEX) per head; slot N (1-based, as CHANGE_ZCOLOR names it)
    /// lives at index N-1.
    std::array<std::pair<std::string, std::string>, 4> zmod_slots_{
        {{"PLA", "FFFFFF"}, {"PETG", "0ACC38"}, {"ABS", "161616"}, {"PLA", "F72224"}}};

    /// Begin a swap to `tool` (MHC_SET), or with -1 park whatever is on the head
    /// (MHC_DROP; @p close_after re-grips the filament afterwards, as DROP_TOOL
    /// does). Clears the failure latch, as the controller's _begin() does;
    /// advance_medusa_swap() does the rest.
    void start_medusa_swap(int tool, bool close_after = true);
    /// pin_watch's current_tool for the simulated pins. Caller holds medusa_mutex_.
    [[nodiscard]] int medusa_pin_tool_locked() const;
    /// The controller's _fail(): latch the error and end the operation. Caller
    /// holds medusa_mutex_, and pauses a running print once it has let go.
    void medusa_fail_locked(std::string message);
    /// The controller's `operation` word for @p phase.
    static const char* medusa_operation_word(MedusaPhase phase);
    /// Whether the last command armed a swap phase; clears the flag.
    bool take_medusa_swap_armed();
    /// Finish a held reply once its swap has ended (main thread).
    void complete_medusa_reply(MedusaReply reply, bool failed, bool print_active,
                               const std::string& message);

    // --- Bondtech INDX mock hardware (moonraker_client_mock_indx.cpp) ----------
    // The macro state Bondtech's upstream macros publish (save_variables and
    // gcode_macro variables), driven by T<n> / CHANGE_TOOL / PARK_TOOL.
    // Written from gcode_script(), read by the RPC handlers and the
    // simulation thread: all behind indx_mutex_.
    struct IndxMockState {
        int tool_count = 6;
        int active_tool = 0;
        int toolchange_count = 0;
        int seat_suspect = -1;
        int toolhead_state = 0; ///< 0 locked, 2 open
        bool docks_calibrated = true;
        std::vector<int> no_heat_tools{5};
        bool spool_macros = false;   ///< T<n> declares spool_id and sets the active spool
        bool kalico = false;         ///< [indx] in configfile only, not in the objects list
        bool unrecognized = false;   ///< CHANGE_TOOL missing
        std::vector<int> spool_ids;  ///< Per tool, with spool_macros
        std::string mode = "normal"; ///< _TOOLCHANGE_VARS.mode
        /// Per tool: the X, Y, Z offset its calibration measured.
        std::vector<std::array<double, 3>> offsets;
        double global_z = -0.05; ///< save_variables global_z_offset
    };
    mutable std::mutex indx_mutex_;
    IndxMockState indx_;
    /// The INDX frame last sent, so a change sends only what moved.
    nlohmann::json indx_published_;

    /// Read the HELIX_MOCK_INDX_* knobs into indx_.
    void read_indx_knobs();
    /// Objects and configfile sections an INDX printer lists.
    void append_indx_objects(nlohmann::json& objects) const;
    [[nodiscard]] nlohmann::json indx_config_sections() const;
    /// Send the INDX objects when they differ from the last frame sent.
    void publish_indx_if_changed();
    /// Park the mounted tool, then pick up @p tool (none for -1) and heat it
    /// to @p target_c when it has a hotend: the end of every change macro.
    void run_indx_change(int tool, double target_c);

    /// Advance the armed swap by one notification interval. No-op when idle.
    void advance_medusa_swap();

    // --- Snapmaker U1 feeder simulation --------------------------------------
    // AUTO_FEEDING (and AUTO_FEEDING_BATCH ACTION=DOING) lines name a channel
    // with EXTRUDER=n; gcode_script() answers each by publishing the
    // channel_state sequence the feeder firmware reports, one full
    // filament_feed frame per state, so the production AmsBackendSnapmaker
    // status parse sees the same transitions a real U1 sends. State starts at
    // the rig's resting mix (heads 0/2 loaded, 1/3 preloaded) so the sibling
    // channel each frame carries alongside the touched one reads plausibly.
    static constexpr int kU1Channels = 4;
    std::array<std::string, static_cast<size_t>(kU1Channels)> u1_channel_state_{
        "load_finish", "preload_finish", "load_finish", "preload_finish"};
    std::array<bool, static_cast<size_t>(kU1Channels)> u1_filament_detected_{true, true, true,
                                                                             true};
    /// defect_detection main_enable and clean_bed.enable, switched by
    /// DEFECT_DETECTION_CONFIG.
    std::atomic<bool> u1_defect_main_enable_{true};
    std::atomic<bool> u1_clean_bed_enable_{true};

    /// One feeder command line: walk the named channel through its states.
    /// HELIX_MOCK_BATCH_FAIL_SLOT=<n> swaps that channel's terminal for its
    /// *_fail variant. @return true when the line was an AUTO_FEEDING command.
    bool apply_u1_feeding_gcode(const std::string& gcode);

    /// Publish one full filament_feed frame for the side owning extruder
    /// `ext`, carrying BOTH of that side's channels with every field — a
    /// present-but-partial channel object resets the fields the backend's
    /// delta parse would otherwise carry forward.
    void publish_u1_channel_frame(int ext);
    /// One filament_feed side's channels object, both of its channels whole.
    [[nodiscard]] nlohmann::json u1_feed_side_json(int ext) const;

    // --- Standalone IFS module mock -----------------------------------------
    // HELIX_MOCK_AMS=ifs-module: pushes the module's objects (ifs /
    // ifs_materials plus its stock-named lane/toolhead sensors) so real
    // discovery sets AmsType::AD5X_IFS and the PRODUCTION AmsBackendAd5xIfs
    // runs against pushed frames — try_create_mock() declines this value, same
    // rule as the MedusaHC modes. gcode_script() folds T<n> and the IFS_*
    // macros into the state below; the periodic notification publishes it.
    bool is_mock_ifs_module() const;
    /// The `ifs` object frame.
    [[nodiscard]] nlohmann::json ifs_module_status_json() const;
    /// The `ifs_materials` object frame (slot registry view).
    [[nodiscard]] nlohmann::json ifs_module_materials_json() const;
    /// The save_variables fragment carrying the module's own records.
    [[nodiscard]] nlohmann::json ifs_module_vars_json() const;
    /// Apply one IFS-module gcode command token. @return true when the token
    /// was one of ours (the caller then stops walking the generic chain).
    bool apply_ifs_module_gcode(const std::string& cmd, const std::string& gcode);

    // --- multiACE mock hardware ----------------------------------------------
    // HELIX_MOCK_AMS=multiace: pushes the U1's own objects plus a plain `ace`
    // so real discovery builds the PRODUCTION AmsBackendMultiAce and runs it
    // against the engine-shaped frames served here — try_create_mock()
    // declines this value, the same rule as the MedusaHC / IFS-module / CFS
    // modes. Pair with HELIX_MOCK_PRINTER=snapmaker_u1. HELIX_MOCK_MULTIACE_MODE
    // picks head|multi and HELIX_MOCK_MULTIACE_UNITS the connected unit count
    // (1-4); ACE_LOAD_HEAD / ACE_UNLOAD_HEAD answer by routing a head to its
    // bay (or clearing it) and walking the head's channel_state the way
    // multiACE's shadow filament_feed does.
    /// Apply one ACE_* engine command. @return true when the line was one.
    bool apply_multiace_gcode(const std::string& gcode);

    /// Routed bay per head ((ace idx, bay), none when the head is unloaded).
    std::array<std::pair<int, int>, 4> multiace_route_{{{-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}}};
    /// The engine's swap phase word, walked while a routed load runs.
    std::string multiace_swap_phase_{"idle"};
    /// HELIX_MOCK_MULTIACE_UNITS, read once at start (1-4).
    int multiace_units_{1};
    /// HELIX_MOCK_MULTIACE_MODE, read once at start ("head" | "multi").
    std::string multiace_mode_{"head"};
    /// Whether @p head is fed from the side feeder: head mode wires one unit
    /// per head, so heads past the unit count have no ACE. Caller holds
    /// multiace_mutex_.
    [[nodiscard]] bool multiace_head_is_feeder(int head) const {
        return multiace_mode_ != "multi" && head >= multiace_units_;
    }
    /// Guards the fields above across the simulation and gcode threads.
    mutable std::mutex multiace_mutex_;
    /// Read the two HELIX_MOCK_MULTIACE_* knobs into the members (idempotent).
    void read_multiace_knobs();

    // --- CFS mock (K1 stock dialect) ----------------------------------------
    // HELIX_MOCK_AMS=cfs: pushes the `box` object so real discovery sets
    // AmsType::CFS and the PRODUCTION AmsBackendCfs runs; try_create_mock()
    // declines this value, same rule as the MedusaHC / IFS-module modes. Pair
    // with HELIX_MOCK_PRINTER=k1 to latch the K1 macro dialect. gcode_script()
    // answers the K1 calibration macros (BOX_FIND_CUT_POS,
    // BOX_CUSTOM_COMMAND) with the response lines a real box firmware sends.
    bool is_mock_cfs() const;
    /// The K2 Plus's `motor_control` and `fan_feedback` frames.
    void append_k2_status(json& status) const;
    // --- OpenAMS mock ---------------------------------------------------------
    // HELIX_MOCK_AMS=openams: pushes `oams_manager` so real discovery claims
    // OpenAMS and the PRODUCTION AmsBackendOpenAms runs (mock hardware, real
    // backend). Slot identity is seeded into the lane_data namespace.
    /// Slot loaded onto the FPS lane, -1 when unloaded.
    std::atomic<int> openams_loaded_slot_{3};
    /// The most units any shape publishes (fleet), and the size of the dryer simulation table.
    static constexpr int kOpenAmsMaxUnits = 12;
    [[nodiscard]] nlohmann::json openams_shared_status_json() const;
    /// What a unit's chamber cools to when its dryer is off.
    [[nodiscard]] static double openams_ambient_c(int unit);
    void service_openams_late_links(uint32_t tick);
    /// A unit fault latched on the AMS HT (HELIX_MOCK_OPENAMS_FAULT=<code>) until
    /// OAMS_CLEAR_FAULT OAMS=1.
    std::atomic<bool> openams_fault_active_{false};
    /// The plugin shape's units' dryers (index 0 = ams_ht, 1 = ams2, ...), driven by
    /// OAMS_DRYER_START / OAMS_DRYER_STOP and counted down by the simulation tick.
    struct OpenAmsDryerSim {
        std::atomic<double> target_c{0.0};
        std::atomic<double> remaining_s{0.0};
        std::atomic<double> chamber_c{27.7};
    };
    OpenAmsDryerSim openams_dryers_[kOpenAmsMaxUnits];
    void service_openams_dryers(double dt_s);
    [[nodiscard]] nlohmann::json openams_device_json(int unit) const;

    /// The `box` object frame, stock K1 shape (T1 unit, four bays).
    [[nodiscard]] nlohmann::json cfs_box_status_json() const;
    /// Emit the BOX_FIND_CUT_POS response lines synchronously inside
    /// gcode_script, so they precede the RPC ack the way streamed gcode
    /// responses do against real hardware (the real sweep takes ~60s).
    void simulate_cfs_find_cut_pos();
    /// Apply one BOX_CUSTOM_COMMAND. @return true when CMD= was one of ours.
    bool apply_cfs_box_custom_command(const std::string& gcode);
    /// Apply the K2 CR_BOX_EXTRUDE / CR_BOX_RETRUDE lines of a load, unload or
    /// swap script to the loaded bay and push the `box` frame.
    /// @return true when the script held one.
    bool apply_cfs_cr_box_script(const std::string& gcode);
    /// Global slot the box reports loaded, or -1 when none is.
    std::atomic<int> cfs_loaded_slot_{-1};

  public:
    /// The toolhead switch sees filament exactly while a bay is loaded.
    [[nodiscard]] bool cfs_toolhead_filament_detected() const {
        return cfs_loaded_slot_.load() >= 0;
    }

    // --- multiACE mock hardware (public: the frame builder is what the
    // parity test reads, beside the sibling mock-hardware builders above). ---
    bool is_mock_multiace() const;
    /// The `ace` object frame, the engine's get_status() shape.
    [[nodiscard]] nlohmann::json multiace_status_json() const;

    // --- Bondtech INDX mock hardware (public for the mock tests) ---
    bool is_mock_indx() const;
    /// The simulated extruder target (0 = off).
    [[nodiscard]] double extruder_target() const {
        return extruder_target_.load();
    }
    /// Every INDX status object, keyed as Klipper names them.
    [[nodiscard]] nlohmann::json indx_status_json() const;

    /// display_status.message as the mock publishes it now: the last M117 once one was
    /// sent (an empty one included), else the print-start chatter for the current phase,
    /// else null.
    [[nodiscard]] json current_display_message() const;
    /// The M117 a typical PRINT_START macro shows during a simulated print-start step
    /// (a SimulatedPrintStartPhase value).
    static const char* print_start_display_message(uint8_t simulated_phase);

  private:
    // --- gcode_script() handlers (moonraker_client_mock_gcode*.cpp) -----------
    // One handler per command family. A handler returns a code to end the
    // script, or std::nullopt to let the checks after it see the same line.
    using GcodeResult = std::optional<int>;
    GcodeResult gcode_skip_wrappers(const std::string& gcode);
    GcodeResult gcode_layer_pause(const std::string& gcode);
    GcodeResult gcode_ifs_module(const std::string& gcode);
    GcodeResult gcode_medusa(const std::string& gcode);
    GcodeResult gcode_indx(const std::string& gcode);
    /// RESPOND: echoes MSG on the console the way Klipper's [respond] does, so
    /// an action prompt a script raises reaches the screen.
    GcodeResult gcode_respond(const std::string& gcode);
    GcodeResult gcode_zmod(const std::string& gcode);
    GcodeResult gcode_cfs(const std::string& gcode);
    GcodeResult gcode_openams(const std::string& script);
    GcodeResult gcode_openams_line(const std::string& gcode);
    GcodeResult gcode_u1_feeding(const std::string& gcode);
    GcodeResult gcode_heater_temperature(const std::string& gcode);
    GcodeResult gcode_temperature_fan_target(const std::string& gcode);
    GcodeResult gcode_panda_dry_start(const std::string& gcode);
    GcodeResult gcode_panda_dry_stop(const std::string& gcode);
    GcodeResult gcode_set_pin(const std::string& gcode);
    GcodeResult gcode_extruder_target_mcode(const std::string& gcode);
    GcodeResult gcode_bed_target_mcode(const std::string& gcode);
    GcodeResult gcode_display_message(const std::string& gcode);
    GcodeResult gcode_save_gcode_state(const std::string& gcode);
    GcodeResult gcode_absolute_mode(const std::string& gcode);
    GcodeResult gcode_relative_mode(const std::string& gcode);
    GcodeResult gcode_disable_motors(const std::string& gcode);
    GcodeResult gcode_home(const std::string& gcode);
    GcodeResult gcode_move(const std::string& gcode);
    GcodeResult gcode_restore_gcode_state(const std::string& gcode);
    GcodeResult gcode_tool_change(const std::string& gcode);
    GcodeResult gcode_sdcard_print_file(const std::string& gcode);
    GcodeResult gcode_pause(const std::string& gcode);
    GcodeResult gcode_resume(const std::string& gcode);
    GcodeResult gcode_cancel_print(const std::string& gcode);
    GcodeResult gcode_emergency_stop(const std::string& gcode);
    GcodeResult gcode_fan_m106(const std::string& gcode);
    GcodeResult gcode_fan_m107(const std::string& gcode);
    GcodeResult gcode_set_fan_speed(const std::string& gcode);
    GcodeResult gcode_pid_calibrate(const std::string& gcode);
    GcodeResult gcode_mpc_calibrate(const std::string& gcode);
    GcodeResult gcode_save_config(const std::string& gcode);
    GcodeResult gcode_bed_mesh(const std::string& gcode);
    GcodeResult gcode_set_gcode_offset(const std::string& gcode);
    GcodeResult gcode_set_tool_parameter(const std::string& gcode);
    GcodeResult gcode_save_tool_parameter(const std::string& gcode);
    GcodeResult gcode_shaper_calibrate(const std::string& gcode);
    GcodeResult gcode_test_resonances(const std::string& gcode);
    GcodeResult gcode_set_led(const std::string& gcode);
    GcodeResult gcode_set_pressure_advance(const std::string& gcode);
    GcodeResult gcode_probe_calibrate(const std::string& gcode);
    GcodeResult gcode_testz(const std::string& gcode);
    GcodeResult gcode_accept(const std::string& gcode);
    GcodeResult gcode_abort(const std::string& gcode);
    GcodeResult gcode_exclude_object(const std::string& gcode);
    GcodeResult gcode_exclude_object_define(const std::string& gcode);
    GcodeResult gcode_set_led_effect(const std::string& gcode);
    GcodeResult gcode_stop_led_effects(const std::string& gcode);
    /// Lane in the nozzle (1-4, 0 = none) — the module's `ifs_loaded` record.
    std::atomic<int> ifs_module_loaded_{0};
    /// The module's `ifs_at_hub` record (1-4, 0 = none): the lane holding the
    /// shared path.
    std::atomic<int> ifs_module_at_hub_{0};
    /// Whether the toolhead switch reads filament. Separate from ifs_loaded,
    /// which the firmware can lose while filament stays in the head.
    std::atomic<bool> ifs_module_head_loaded_{false};
    /// Per-lane silk presence bitmask, bit i = lane i+1.
    std::atomic<uint8_t> ifs_module_presence_{0x0F};
    /// Slot -> {type, bare-hex colour}; IFS_SET_MATERIAL writes land here.
    mutable std::mutex ifs_module_materials_mutex_;
    std::map<int, std::pair<std::string, std::string>> ifs_module_materials_{
        {1, {"PLA", "A03CF7"}},
        {2, {"PETG", "00FF00"}},
        {3, {"ABS", "FF8800"}},
        {4, {"TPU", "FFFFFF"}}};

    // Additional objects for testing (e.g., "mmu", "AFC", "toolchanger")
    std::vector<std::string> additional_objects_;

    // Cached chamber heater status key (updated by override_chamber_heater / populate)
    std::string cached_chamber_status_key_;

    // Chamber filter-fan pin value (SET_PIN target; drives fan_percent in
    // synthesized chamber-backend diagnostics frames)
    std::atomic<double> chamber_filter_value_{0.0};

    // Chamber appliance drying cycle (stock panda_breath DRY_START/DRY_STOP).
    // The countdown runs on the simulated clock: the tick loop advances it at
    // the simulation speed, a DRY_START stamps it as the cycle's start.
    std::atomic<double> mock_sim_time_{0.0};
    std::atomic<bool> chamber_drying_{false};
    std::atomic<int> chamber_dry_temp_{0};
    std::atomic<int> chamber_dry_hours_{0};
    std::atomic<double> chamber_dry_start_{0.0};

    // Chamber appliance test hooks (HELIX_MOCK_DRAGONBREATH_* and
    // HELIX_MOCK_PANDA_BREATH_*). Read from the environment once, at
    // construction: the simulation thread builds a frame every tick, and a
    // getenv() there races a setenv() on the main thread. A test that crosses
    // a transition on one client flips the atomic through
    // MoonrakerClientMockTestAccess instead.
    std::atomic<bool> dragonbreath_fault_{false};
    std::atomic<bool> dragonbreath_offline_{false};
    std::atomic<bool> dragonbreath_external_{false};
    /// HELIX_MOCK_DRAGONBREATH_REJECT: the vendor code every chamber target is
    /// refused with ("" = targets are accepted). Written once at construction.
    std::string dragonbreath_reject_code_;
    std::atomic<int> dragonbreath_reject_seq_{0};
    std::atomic<double> dragonbreath_reject_target_{0.0};
    std::atomic<bool> dragonbreath_rejection_outstanding_{false};
    std::atomic<bool> panda_breath_offline_{false};
    std::atomic<bool> panda_breath_auto_{false};

    // Calibration simulation timers (PID, MPC, shaper) — must be cleaned up
    // in destructor to prevent use-after-free when mock is destroyed before
    // LVGL timers fire in a subsequent test's process_lvgl(). Each entry also
    // owns its heap payload: the timer callbacks self-delete at natural
    // completion, but a forced teardown that only lv_timer_delete()s the timer
    // leaks the payload (the 80-byte ShaperSimState the 2026-08-18 nightly's
    // leak report showed surviving the suite).
    struct CalibrationTimer {
        lv_timer_t* timer = nullptr;
        helix::SharedCallback<void()> free_payload;
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
