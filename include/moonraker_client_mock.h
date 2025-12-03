// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

/*
 * Copyright (C) 2025 356C LLC
 * Author: Preston Brown <pbrown@brown-house.net>
 *
 * This file is part of HelixScreen.
 *
 * HelixScreen is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * HelixScreen is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with HelixScreen. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef MOONRAKER_CLIENT_MOCK_H
#define MOONRAKER_CLIENT_MOCK_H

#include "moonraker_client.h"

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

// Forward declaration for shared state
class MockPrinterState;

/**
 * @brief Mock Moonraker client for testing without real printer connection
 *
 * Simulates printer hardware discovery with configurable test data.
 * Useful for UI development and testing without physical hardware.
 *
 * Inherits from MoonrakerClient to provide drop-in replacement compatibility.
 * Overrides discover_printer() to populate test data without WebSocket connection.
 */
class MoonrakerClientMock : public MoonrakerClient {
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
        double estimated_time_seconds = 300.0; ///< Default 5 min if not in file
        uint32_t layer_count = 100;            ///< Default 100 layers
        double target_bed_temp = 60.0;         ///< First layer bed temp
        double target_nozzle_temp = 210.0;     ///< First layer nozzle temp
        double filament_mm = 0.0;              ///< Total filament length

        void reset() {
            estimated_time_seconds = 300.0;
            layer_count = 100;
            target_bed_temp = 60.0;
            target_nozzle_temp = 210.0;
            filament_mm = 0.0;
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
     *
     * @param on_complete Callback invoked after discovery completes
     */
    void discover_printer(std::function<void()> on_complete) override;

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
    RequestId send_jsonrpc(const std::string& method, const json& params,
                           std::function<void(json)> cb) override;

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
     * @return Always returns 0 (success)
     */
    RequestId send_jsonrpc(const std::string& method, const json& params,
                           std::function<void(json)> success_cb,
                           std::function<void(const MoonrakerError&)> error_cb,
                           uint32_t timeout_ms = 0) override;

    /**
     * @brief Simulate G-code script command
     *
     * Overrides base class to log and return success without network I/O.
     *
     * @param gcode G-code string
     * @return Always returns 0 (success)
     */
    int gcode_script(const std::string& gcode) override;

    /**
     * @brief Set printer type for mock data generation
     *
     * @param type Printer type to simulate
     */
    void set_printer_type(PrinterType type) {
        printer_type_ = type;
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
     */
    void stop_temperature_simulation();

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

  private:
    /**
     * @brief Populate hardware lists based on configured printer type
     *
     * Directly modifies the protected member variables inherited from
     * MoonrakerClient (heaters_, sensors_, fans_, leds_).
     */
    void populate_hardware();

    /**
     * @brief Generate synthetic bed mesh data for testing
     *
     * Creates a realistic dome-shaped mesh (7×7 points, 0-0.3mm Z range).
     * Populates active_bed_mesh_ with test data compatible with renderer.
     */
    void generate_mock_bed_mesh();

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

    // ========== Unified Print Control (internal implementation) ==========
    // These methods are called by BOTH G-code commands AND JSON-RPC API

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
     * Called by both PAUSE G-code and printer.print.pause JSON-RPC.
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
     * @brief Generate next mock request ID
     * @return Valid request ID (always > 0)
     */
    RequestId next_mock_request_id() {
        return mock_request_id_counter_.fetch_add(1) + 1;
    }

  private:
    PrinterType printer_type_;

    // Mock request ID counter for simulating send_jsonrpc return values
    std::atomic<RequestId> mock_request_id_counter_{0};

    // Temperature simulation state
    std::atomic<double> extruder_temp_{25.0};  // Current temperature
    std::atomic<double> extruder_target_{0.0}; // Target temperature (0 = off)
    std::atomic<double> bed_temp_{25.0};       // Current temperature
    std::atomic<double> bed_target_{0.0};      // Target temperature (0 = off)

    // Position simulation state
    std::atomic<double> pos_x_{0.0};
    std::atomic<double> pos_y_{0.0};
    std::atomic<double> pos_z_{0.0};

    // Motion mode state
    std::atomic<bool> relative_mode_{false}; // G90=absolute (false), G91=relative (true)

    // Homing state (needs mutex since std::string is not atomic)
    mutable std::mutex homed_axes_mutex_;
    std::string homed_axes_;

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

    // Excluded objects tracking (for EXCLUDE_OBJECT command)
    std::set<std::string> excluded_objects_; // Object names excluded during print (local fallback)
    mutable std::mutex excluded_objects_mutex_; // Protects excluded_objects_

    // Shared mock state for coordination with MoonrakerAPIMock
    // When set, state changes are propagated to this shared object
    std::shared_ptr<MockPrinterState> mock_state_;

    // Simulation tick counter
    std::atomic<uint32_t> tick_count_{0};

    // Simulation thread control
    std::thread simulation_thread_;
    std::atomic<bool> simulation_running_{false};

    // Restart simulation thread (for RESTART/FIRMWARE_RESTART commands)
    std::thread restart_thread_;
    std::atomic<bool> restart_pending_{false};

    // Simulation parameters (realistic heating rates)
    static constexpr double ROOM_TEMP = 25.0;
    static constexpr double EXTRUDER_HEAT_RATE = 3.0;  // °C/sec when heating
    static constexpr double EXTRUDER_COOL_RATE = 1.5;  // °C/sec when cooling
    static constexpr double BED_HEAT_RATE = 1.0;       // °C/sec when heating
    static constexpr double BED_COOL_RATE = 0.3;       // °C/sec when cooling
    static constexpr int SIMULATION_INTERVAL_MS = 500; // Update frequency
};

#endif // MOONRAKER_CLIENT_MOCK_H
