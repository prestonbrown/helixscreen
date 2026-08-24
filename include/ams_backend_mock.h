// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_backend.h"
#include "slot_registry.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

/**
 * @file ams_backend_mock.h
 * @brief Mock AMS backend for development and testing
 *
 * Provides a simulated multi-filament system with configurable slots,
 * fake operation timing, and predictable state for UI development.
 *
 * Features:
 * - Configurable slot count (default 4)
 * - Simulated load/unload timing
 * - Pre-populated filament colors and materials
 * - Responds to all AmsBackend operations
 */
class AmsBackendMock : public AmsBackend {
  public:
    /**
     * @brief Construct mock backend with specified slot count
     * @param slot_count Number of simulated slots (1-16, default 4)
     */
    explicit AmsBackendMock(int slot_count = 4);

    ~AmsBackendMock() override;

    // Lifecycle
    AmsError start() override;
    void stop() override;
    [[nodiscard]] bool is_running() const override;

    // Guarded to match the base declaration: ESP32 compiles this class for the
    // CONFIG_HELIX_MOCK_PRINTER build but never defines HELIX_ENABLE_MOCKS, so
    // there is no base virtual to override there.
#ifdef HELIX_ENABLE_MOCKS
    [[nodiscard]] AmsBackendMock* as_mock() override {
        return this;
    }
#endif

    // Mock-only: receiver for MoonrakerClientMock's active-gcode-tool
    // notifications. Wired up by MoonrakerManager when both mocks are live.
    // Production AMS backends get equivalent state from real Klipper via
    // printer.mmu.tool / toolchanger.tool_number subscriptions.
    //
    // slicer_color_rgb: fallback color from the gcode header — used when the
    // active tool isn't mapped to a real slot (avoids the swatch lying about
    // slot 0's color). Pass 0 if no slicer color metadata is available.
    void on_simulated_gcode_tool_changed(int tool_index, uint32_t slicer_color_rgb = 0);

    // Events
    void set_event_callback(EventCallback callback) override;

    // State queries
    [[nodiscard]] AmsSystemInfo get_system_info() const override;
    [[nodiscard]] AmsType get_type() const override;
    [[nodiscard]] SlotInfo get_slot_info(int slot_index) const override;
    [[nodiscard]] AmsAction get_current_action() const override;
    [[nodiscard]] int get_current_tool() const override;
    [[nodiscard]] int get_current_slot() const override;
    [[nodiscard]] bool is_filament_loaded() const override;

    // Capability flag (overridable in tests; production mock returns default false)
    [[nodiscard]] bool tracks_consumption_natively() const override {
        return tracks_consumption_natively_;
    }

    /// Mock extruder->slot mapping. Returns slot == extruder_idx when the
    /// identity mapping flag is set (simulates a tool-changer) and
    /// std::nullopt otherwise (default AmsBackend behavior — aggregate routing).
    [[nodiscard]] std::optional<int> slot_for_extruder(int extruder_idx) const override {
        if (!identity_extruder_mapping_) {
            return std::nullopt;
        }
        if (extruder_idx < 0 || extruder_idx >= system_info_.total_slots) {
            return std::nullopt;
        }
        return extruder_idx;
    }

    /**
     * @brief Test hook: make get_slot_for_extruder / tracks_consumption_natively() return
     *        the given value. Test-only setter on the mock, not on a production backend
     *        (per CLAUDE L065 the mock is test infrastructure).
     */
    void set_tracks_consumption_natively_for_testing(bool value) {
        tracks_consumption_natively_ = value;
    }

    /**
     * @brief Test hook: enable identity extruder->slot mapping so routing tests
     *        can exercise the per-extruder dispatch path. When true,
     *        slot_for_extruder(i) returns i; when false (default),
     *        slot_for_extruder returns nullopt to preserve the pre-Phase-5
     *        aggregate-routing behavior.
     */
    void set_identity_extruder_mapping_for_testing(bool enabled) {
        identity_extruder_mapping_ = enabled;
    }

    // Path visualization
    [[nodiscard]] PathTopology get_topology() const override;
    [[nodiscard]] PathTopology get_unit_topology(int unit_index) const override;
    [[nodiscard]] PathSegment get_filament_segment() const override;
    [[nodiscard]] PathSegment get_slot_filament_segment(int slot_index) const override;
    [[nodiscard]] PathSegment infer_error_segment() const override;
    [[nodiscard]] bool slot_has_prep_sensor(int slot_index) const override;

    // Operations
    AmsError load_filament(int slot_index) override;
    AmsError unload_filament(int slot_index) override;
    AmsError select_slot(int slot_index) override;
    AmsError change_tool(int tool_number) override;

    // Recovery
    AmsError recover() override;
    AmsError reset() override;
    AmsError cancel() override;
    AmsError clear_fault(int slot_index) override;

    // Gate select / check (Happy Hare selector-based systems only)
    AmsError select_gate(int slot_index) override;
    AmsError move_selector(int delta) override;
    [[nodiscard]] bool supports_gate_select() const override {
        return system_info_.type == AmsType::HAPPY_HARE;
    }
    AmsError check_gate(int slot_index) override;
    AmsError check_all_gates() override;
    [[nodiscard]] bool supports_gate_check() const override {
        return system_info_.type == AmsType::HAPPY_HARE;
    }
    [[nodiscard]] std::string reset_button_label() const override {
        return system_info_.type == AmsType::HAPPY_HARE ? std::string("Home")
                                                        : std::string("Reset");
    }

    // Configuration

    /**
     * @brief Edit a lane's FILAMENT metadata. SlotInfo::status is IGNORED.
     *
     * Same contract as the real backends: status is firmware-derived, so this
     * path copies color / material / brand / Spoolman / weights / temps and the
     * tool mapping, and nothing else. Passing a status other than UNKNOWN that
     * differs from the slot's current one logs a warning rather than silently
     * doing nothing — use force_slot_status() to stage a mock slot state.
     */
    AmsError set_slot_info(int slot_index, const SlotInfo& info, bool persist = true) override;
    AmsError set_tool_mapping(int tool_number, int slot_index) override;

    // Bypass mode
    AmsError enable_bypass() override;
    AmsError disable_bypass() override;
    [[nodiscard]] bool is_bypass_active() const override;

    /// nullopt until the "unaccounted" scenario runs; then true — the mock
    /// cannot observe a toolhead, so only the staged scenario answers.
    [[nodiscard]] std::optional<bool> toolhead_filament_unaccounted() const override;

    // Environment sensors
    [[nodiscard]] bool has_environment_sensors() const override;

    // Dryer
    [[nodiscard]] DryerInfo get_dryer_info(int unit = 0) const override;
    AmsError start_drying(float temp_c, int duration_min, int fan_pct = -1, int unit = 0) override;
    AmsError stop_drying(int unit = 0) override;

    // Endless spool. The mock keeps its edges in the SlotRegistry, so its config
    // is the same one-liner AFC uses. reset_endless_spool() is NOT overridden -
    // the base's clear-every-slot loop is exactly the behaviour the mock wants,
    // and its absence is why the mock used to advertise editable=true while
    // returning NOT_SUPPORTED from reset.
    /// @note Takes `mutex_`; callers must NOT hold it.
    [[nodiscard]] helix::printer::EndlessSpoolCapabilities
    get_endless_spool_capabilities() const override;
    /// @note Takes `mutex_`; callers must NOT hold it.
    [[nodiscard]] helix::printer::EndlessSpoolConfig get_endless_spool_config() const override;

    // Tool mapping
    [[nodiscard]] helix::printer::ToolMappingCapabilities
    get_tool_mapping_capabilities() const override;
    [[nodiscard]] std::vector<int> get_tool_mapping() const override;
    [[nodiscard]] RemapStrategy get_remap_strategy() const override;
    // Mirrors get_remap_strategy(): when emulating Snapmaker U1 the controller
    // must run the pre-print send path (a no-op here since the mock does not
    // override build_preprint_gcode, matching the old strategy-gated behavior).
    [[nodiscard]] bool requires_preprint_send() const override;

    // Device actions
    [[nodiscard]] std::vector<helix::printer::DeviceSection> get_device_sections() const override;
    [[nodiscard]] std::vector<helix::printer::DeviceAction> get_device_actions() const override;
    AmsError execute_device_action(const std::string& action_id,
                                   const std::any& value = {}) override;

    // ========================================================================
    // Mock-specific methods (for testing)
    // ========================================================================

    /**
     * @brief Simulate an error condition
     * @param error The error to trigger
     */
    void simulate_error(AmsResult error);

    /**
     * @brief Simulate a paused state (user intervention required)
     *
     * Sets the action to PAUSED, which can be resumed with resume().
     * Used to test UI handling of pause scenarios.
     */
    void simulate_pause();

    /**
     * @brief Resume from PAUSED state
     * @return Success, or error if not in PAUSED state
     *
     * Returns to IDLE state. No-op if already IDLE.
     */
    AmsError resume();

    /**
     * @brief Set operation delay for simulated timing
     * @param delay_ms Delay in milliseconds (0 for instant)
     */
    void set_operation_delay(int delay_ms);

    /**
     * @brief Block until the current simulated operation has finished
     *
     * Every load / unload / tool change runs on a real std::thread spawned by
     * schedule_completion(), so the transient AmsAction a caller sees right after
     * issuing one is a race against that thread — and at set_operation_delay(0)
     * the thread routinely wins, ending back at IDLE before the caller looks.
     *
     * Tests that want to assert on the RESULT of an operation must join here
     * first and then read the settled state, rather than sampling the action mid
     * flight. Public because the whole class is test infrastructure; joining is
     * the only way to make those assertions deterministic under parallel load.
     */
    void wait_for_operation_thread();

    /**
     * @brief Force a specific slot status (for testing)
     * @param slot_index Slot to modify
     * @param status New status
     */
    void force_slot_status(int slot_index, SlotStatus status);

    /**
     * @brief Set per-slot error state (for testing error visualization)
     * @param slot_index Slot to modify
     * @param error Error to set, or nullopt to clear
     */
    void set_slot_error(int slot_index, std::optional<SlotError> error);

    /**
     * @brief Set unit-level buffer health (for testing hub buffer visualization)
     * @param unit_index Unit to modify
     * @param health Buffer health to set, or nullopt to clear
     */
    void set_unit_buffer_health(int unit_index, std::optional<BufferHealth> health);

    /**
     * @brief Inject error states for visual testing (HELIX_MOCK_AMS_STATE=error)
     *
     * Adds lane errors and buffer fault warnings to existing units for
     * testing error visualization (slot error dots, hub tint, overview badges).
     */
    void inject_mock_errors();

    /**
     * @brief Set whether this mock simulates a hardware bypass sensor
     * @param has_sensor true=hardware sensor (auto-detect), false=virtual (manual toggle)
     *
     * When has_sensor is true:
     * - The bypass button should be disabled in the UI
     * - Bypass is controlled by the sensor, not user clicks
     */
    void set_has_hardware_bypass_sensor(bool has_sensor);

    /**
     * @brief Set environment sensor simulation mode
     * @param mode One of: "passive", "dryer", "slot", "" (auto-detect)
     *
     * Controls mock environment data on units/slots:
     * - "passive": Per-unit temp/humidity, no dryer capability
     * - "dryer": Per-unit temp/humidity + dryer supported (default when dryer enabled)
     * - "slot": Per-slot temp/humidity (different values per slot)
     * - "" or unset: auto-detect from dryer state (dryer enabled → "dryer", else "passive")
     *
     * Can also be set via HELIX_MOCK_AMS_ENV environment variable.
     */
    void set_environment_mode(const std::string& mode);

    /**
     * @brief Enable dryer simulation for this mock
     * @param enabled true to simulate a dryer, false to disable
     *
     * When enabled, the mock will:
     * - Report dryer_supported = true in get_dryer_info()
     * - Simulate temperature ramping and progress when drying
     * - Support start_drying() and stop_drying() commands
     */
    void set_dryer_enabled(bool enabled);

    /**
     * @brief Set dryer simulation speed multiplier
     * @param speed_x Speed multiplier (default 60 = 1 real second = 1 simulated minute)
     *
     * Can also be set via HELIX_MOCK_DRYER_SPEED environment variable.
     * Set to 1 for real-time, 60 for fast testing (4h = 4min), 3600 for instant.
     */
    void set_dryer_speed(int speed_x);

    /**
     * @brief Give the next start_drying() a head start (mock/demo only)
     * @param min Simulated minutes already elapsed when drying begins (clamped to
     *            the session duration). Consumed by the next start_drying() call,
     *            then reset to 0. Lets a mock session begin partway through so the
     *            countdown and progress bar read consistently.
     */
    void set_dryer_initial_elapsed_min(int min);

    /**
     * @brief Enable realistic multi-phase operation mode
     * @param enabled true for HEATING→LOADING→CHECKING sequences
     *
     * When enabled, operations show realistic phase progression:
     * - Load: HEATING → LOADING (segment animation) → CHECKING → IDLE
     * - Unload: HEATING → CUTTING → UNLOADING (animation) → IDLE
     *
     * Can also be set via HELIX_MOCK_AMS_STATE=loading environment variable.
     * Timing respects --sim-speed flag with ±20-30% variance.
     */
    void set_realistic_mode(bool enabled);

    /**
     * @brief Check if realistic mode is enabled
     * @return true if multi-phase operations are simulated
     */
    [[nodiscard]] bool is_realistic_mode() const;

    /**
     * @brief Enable tool changer simulation mode
     * @param enabled true to simulate a tool changer, false for filament system
     *
     * When enabled, the mock will:
     * - Report type as TOOL_CHANGER instead of HAPPY_HARE
     * - Use PARALLEL path topology (each slot → own toolhead)
     * - Disable bypass mode (not applicable for tool changers)
     * - Label slots as "T0", "T1", etc.
     *
     * Can also be set via HELIX_MOCK_AMS=toolchanger environment variable.
     */
    void set_tool_changer_mode(bool enabled);

    /**
     * @brief Check if tool changer mode is enabled
     * @return true if simulating a tool changer
     */
    [[nodiscard]] bool is_tool_changer_mode() const;

    /**
     * @brief Enable AFC simulation mode
     * @param enabled true to simulate an AFC Box Turtle, false for Happy Hare
     *
     * When enabled, the mock will:
     * - Report type as AFC instead of HAPPY_HARE
     * - Use HUB path topology (4 lanes merge through hub)
     * - Configure 4 lanes with realistic AFC filament data
     * - Set AFC-specific device sections and actions (calibration, maintenance, etc.)
     * - Use CUT tip method
     *
     * Can also be set via HELIX_MOCK_AMS=afc environment variable.
     */
    void set_afc_mode(bool enabled);

    /**
     * @brief Check if AFC mode is enabled
     * @return true if simulating an AFC Box Turtle
     */
    [[nodiscard]] bool is_afc_mode() const;

    /**
     * @brief Report as an AFC system while AFC mode is on.
     *
     * The mock claimed to be a Box Turtle in every other respect but left this
     * at the base-class default, so `HELIX_MOCK_AMS=afc` produced a system that
     * looked like AFC to the panels and not-AFC to anything asking this. The
     * AFC-only rows in the device-operations overlay were therefore invisible in
     * the mock, and #1229's bypass rule could not be exercised there at all.
     */
    [[nodiscard]] bool is_afc_system() const override {
        return is_afc_mode();
    }

    /**
     * @brief Enable multi-unit mode for testing overview panel
     *
     * Creates a Box Turtle (4 slots) + Night Owl (2 slots) = 6 total slots.
     * All units feed a single toolhead via combiner (virtual tools).
     *
     * @param enabled true to enable multi-unit, false to revert to single unit
     */
    void set_multi_unit_mode(bool enabled);

    /**
     * @brief Enable the multi-unit torture profile (HELIX_MOCK_AMS=torture).
     *
     * Five units / 16 lanes / 4 Klipper extruders, modelled on a user rig
     * captured 2026-08-16: Box Turtle + Claymore both feed e0, ViViD + EMU both
     * feed e3, and a 2-tool Toolchanger sits between them. Two lanes are
     * deliberately unmapped and the AFC tool aliases are neither dense nor
     * unit-ordered.
     *
     * This is the only profile whose unit-card row overflows its container, so
     * it is the only way to exercise the overview's card-row scrolling, its
     * off-canvas connector clamping, and shared-nozzle hub placement. Every
     * other profile tops out at 3 units, which always fit.
     */
    void set_torture_mode(bool enabled);

    /**
     * @brief Check if multi-unit mode is active
     */
    [[nodiscard]] bool is_multi_unit_mode() const;

    /**
     * @brief Enable mixed topology mode for testing multi-unit mixed hardware
     *
     * Simulates J0eB0l's real hardware: 6-tool toolchanger with mixed AFC:
     * - Unit 0: "Turtle_1" (Box Turtle) - 4 lanes, PARALLEL, 1:1 lane->tool
     * - Unit 1: "AMS_1" (OpenAMS) - 4 lanes, HUB, 4:1 lane->T4
     * - Unit 2: "AMS_2" (OpenAMS) - 4 lanes, HUB, 4:1 lane->T5
     *
     * @param enabled true to enable mixed topology mode
     */
    void set_mixed_topology_mode(bool enabled);

    /**
     * @brief Check if mixed topology mode is active
     */
    [[nodiscard]] bool is_mixed_topology_mode() const;

    /**
     * @brief Enable ViViD mixed mode for testing 2x BoxTurtle + 1x ViViD
     *
     * Simulates the user's real hardware:
     * - Unit 0: "Turtle_1" (Box Turtle) - lanes 1-4, HUB topology
     * - Unit 1: "Turtle_2" (Box Turtle) - lanes 5-8, HUB topology (shared hub)
     * - Unit 2: "vivid_1" (ViViD) - lanes 13-16, HUB topology
     *
     * @param enabled true to enable ViViD mixed mode
     */
    void set_vivid_mixed_mode(bool enabled);

    /**
     * @brief Set AD5X IFS mode (4 slots, LINEAR, bypass + tool mapping)
     */
    void set_ifs_mode(bool enabled);

    /**
     * @brief Set HTLF + Toolchanger mixed topology mode
     *
     * Simulates the user's real HTLF+TC setup: HTLF_1 (4 lanes, MIXED — 2 direct + 2 hub-routed)
     * and Toolchanger Tools (3 standalone toolheads, PARALLEL).
     *
     * @param enabled true to enable HTLF+Toolchanger mode
     */
    void set_htlf_toolchanger_mode(bool enabled);

    /**
     * @brief Set Snapmaker U1 SnapSwap mode (4 slots, PARALLEL, NON-editable
     *        tool mapping, SnapmakerNative remap strategy).
     *
     * Mirrors AmsBackendSnapmaker so the editable FilamentMappingCard hides and
     * the print-detail color_swatches_row renders the two-tone chips, exactly as
     * on the device. Test/dev only (gated by HELIX_MOCK_AMS=snapmaker).
     */
    void set_snapmaker_mode(bool enabled);

    /**
     * @brief Seed per-tool→slot firmware mappings from a "tool:slot" CSV.
     *
     * e.g. "0:3,2:1" maps tool 0 to slot 3 and tool 2 to slot 1 by setting each
     * target slot's mapped_tool. FilamentMapper::compute_defaults() then resolves
     * those tools to those slots (Priority 1: firmware mapping). Test/dev only
     * (gated by HELIX_MOCK_REMAP).
     */
    void apply_remap_overrides(const std::string& csv);

    /**
     * @brief Check if ViViD mixed mode is active
     */
    [[nodiscard]] bool is_vivid_mixed_mode() const;

    /**
     * @brief Check if HTLF+Toolchanger mode is active
     */
    [[nodiscard]] bool is_htlf_toolchanger_mode() const;

    /**
     * @brief Set whether endless spool is supported
     * @param supported true to enable endless spool support
     *
     * When disabled, get_endless_spool_capabilities() reports
     * EndlessSpoolAvailability::Unsupported.
     */
    void set_endless_spool_supported(bool supported);

    /**
     * @brief Set whether endless spool configuration is editable
     * @param editable true for AFC-style (PerSlot), false for read-only
     *
     * When editable=false, set_endless_spool_backup() and reset_endless_spool()
     * are refused by the base with the FirmwareManaged restriction.
     */
    void set_endless_spool_editable(bool editable);

    /**
     * @brief Set mock device sections for testing
     * @param sections Device sections to return from get_device_sections()
     */
    void set_device_sections(std::vector<helix::printer::DeviceSection> sections);

    /**
     * @brief Set mock device actions for testing
     * @param actions Device actions to return from get_device_actions()
     */
    void set_device_actions(std::vector<helix::printer::DeviceAction> actions);

    /**
     * @brief Get the last executed device action (for test verification)
     * @return Pair of (action_id, value) from last execute_device_action() call
     */
    [[nodiscard]] std::pair<std::string, std::any> get_last_executed_action() const;

    /**
     * @brief Clear the last executed action state
     */
    void clear_last_executed_action();

    /**
     * @brief Set tool change progress for testing swap count display
     * @param current Current tool change number (0-based, -1=none yet)
     * @param total Total expected tool changes this print
     */
    void set_toolchange_progress(int current, int total);

    /**
     * @brief Set callback for injecting gcode response lines
     *
     * Used by the mock to simulate Klipper gcode responses (e.g., action:prompt
     * messages) without a real WebSocket connection. The callback feeds lines
     * into ActionPromptManager::process_line() via AmsState.
     *
     * @param callback Function that receives "// action:..." lines
     */
    void set_gcode_response_callback(std::function<void(const std::string&)> callback) override;

    /**
     * @brief Set a deferred state scenario to apply when start() is called
     *
     * Some mock states (loading, bypass) require the backend to be running
     * before they can be applied. This stores the scenario name and applies
     * it at the end of start().
     *
     * @param scenario One of: "idle", "loading", "error", "bypass", "unaccounted"
     */
    void set_initial_state_scenario(const std::string& scenario);

  protected:
    /**
     * @brief "Transport" for the mock: write the edge straight to the registry.
     *
     * Every guard the mock used to carry (support, editability, both ranges,
     * self-backup) now lives in AmsBackend::set_endless_spool_backup().
     *
     * @note Takes `mutex_`; the base calls this with no lock held.
     */
    AmsError apply_endless_spool_backup(int slot_index, int backup_slot) override;

  private:
    /**
     * @brief Initialize mock state with sample data
     */
    void init_mock_data();

    /**
     * @brief Emit event to registered callback
     * @param event Event name
     * @param data Event data (JSON or empty)
     */
    void emit_event(const std::string& event, const std::string& data = "");

    /**
     * @brief Simulate async operation completion
     * @param action Action being performed
     * @param complete_event Event to emit on completion
     * @param slot_index Slot involved (-1 if N/A)
     */
    void schedule_completion(AmsAction action, const std::string& complete_event,
                             int slot_index = -1);

    /**
     * @brief Simulate a transient action state that returns to IDLE.
     *
     * Mirrors the reset() idiom: sets the given action + detail, emits a
     * state-change event, then schedules the action to clear back to IDLE
     * after the standard operation delay. Used by selector-context commands
     * (check/select/jog/servo) so --test surfaces the same AMS status-display
     * feedback that real Happy Hare reports automatically. No-op (returns
     * busy) if an action is already in flight.
     *
     * @param action Transient action enum (CHECKING / SELECTING / RESETTING…)
     * @param detail Operation detail string shown in the status display
     * @return success once the transient state has been scheduled, busy if
     *         the backend is already mid-operation
     */
    AmsError simulate_transient_action(AmsAction action, const std::string& detail);

    // Realistic mode helpers (multi-phase operations)
    using InterruptibleSleep = std::function<bool(int)>;

    /**
     * @brief Get delay with speedup and optional variance applied
     * @param base_ms Base delay in milliseconds (at 1x speed)
     * @param variance Variance factor (0.2 = ±20%, 0 = no variance)
     * @return Effective delay considering RuntimeConfig::sim_speedup
     */
    int get_effective_delay_ms(int base_ms, float variance = 0.0f) const;

    /**
     * @brief Update action state with thread safety
     * @param action New action state
     * @param detail Operation detail string
     */
    void set_action(AmsAction action, const std::string& detail);

    /**
     * @brief Execute load operation with optional multi-phase sequence
     * @param slot_index Slot being loaded from
     * @param interruptible_sleep Sleep function that respects shutdown
     */
    void execute_load_operation(int slot_index, InterruptibleSleep interruptible_sleep);

    /**
     * @brief Execute unload operation with optional multi-phase sequence
     * @param interruptible_sleep Sleep function that respects shutdown
     */
    void execute_unload_operation(InterruptibleSleep interruptible_sleep);

    /**
     * @brief Animate filament through load path segments
     * @param slot_index Slot being loaded from
     * @param interruptible_sleep Sleep function that respects shutdown
     */
    void run_load_segment_animation(int slot_index, InterruptibleSleep interruptible_sleep);

    /**
     * @brief Animate filament through unload path segments (reverse)
     * @param interruptible_sleep Sleep function that respects shutdown
     */
    void run_unload_segment_animation(InterruptibleSleep interruptible_sleep);

    /**
     * @brief Finalize state after successful load
     * @param slot_index Slot that was loaded
     */
    void finalize_load_state(int slot_index);

    /**
     * @brief Finalize state after successful unload
     */
    void finalize_unload_state();

    /**
     * @brief Execute tool change operation with SELECTING phase
     * @param target_slot Target slot for tool change
     * @param interruptible_sleep Sleep function that respects shutdown
     */
    void execute_tool_change_operation(int target_slot, InterruptibleSleep interruptible_sleep);

    /**
     * @brief Schedule recovery sequence (ERROR → CHECKING → IDLE)
     * Runs asynchronously in background thread
     */
    void schedule_recovery_sequence();

    /**
     * @brief Populate environment data on units/slots based on environment_mode_
     *
     * Called from get_system_info() to overlay environment sensor readings.
     * Must be called with mutex_ held.
     */
    void populate_environment_data(AmsSystemInfo& info) const;

    /**
     * @brief Resolve the effective environment mode
     * @return Resolved mode: "passive", "dryer", "slot", or "none"
     *
     * When environment_mode_ is empty, auto-detects from dryer state.
     */
    [[nodiscard]] std::string resolve_environment_mode() const;

    mutable std::mutex mutex_;         ///< Protects state access
    std::atomic<bool> running_{false}; ///< Backend running state
    EventCallback event_callback_;     ///< Registered event handler

    helix::printer::SlotRegistry slots_; ///< Slot registry (single source of truth for slot data)
    AmsSystemInfo system_info_;          ///< Non-slot system metadata
    int operation_delay_ms_ = 5000;      ///< Simulated operation delay
    bool realistic_mode_ = false; ///< Enable multi-phase operations (HEATING→LOADING→CHECKING)

    // Path visualization state
    PathTopology topology_ = PathTopology::LINEAR; ///< Simulated topology (default linear for HH)
    PathSegment filament_segment_ = PathSegment::NONE; ///< Current filament position
    PathSegment error_segment_ = PathSegment::NONE;    ///< Error location (if any)

    // Thread-safe shutdown support
    std::thread operation_thread_;                      ///< Current operation thread (if any)
    std::atomic<bool> operation_thread_running_{false}; ///< Guards against double-join
    std::atomic<bool> shutdown_requested_{false};       ///< Signal thread to exit
    std::atomic<bool> cancel_requested_{false};         ///< Signal operation cancellation
    std::condition_variable shutdown_cv_;               ///< For interruptible sleep
    mutable std::mutex shutdown_mutex_;                 ///< Protects shutdown_cv_ wait

    // Environment sensor simulation
    std::string environment_mode_; ///< "passive", "dryer", "slot", or "" (auto)

    // Dryer simulation state
    bool dryer_enabled_ = false;                    ///< Whether dryer is simulated
    int dryer_initial_elapsed_min_ = 0;             ///< One-shot head start for next start_drying()
    DryerInfo dryer_state_;                         ///< Current dryer state
    std::thread dryer_thread_;                      ///< Background thread for dryer simulation
    std::atomic<bool> dryer_thread_running_{false}; ///< Guards against double-join
    std::atomic<bool> dryer_stop_requested_{false}; ///< Signal dryer thread to stop
    int dryer_speed_x_ = 60; ///< Speed multiplier (60 = 1 real sec = 1 sim min)

    // Tool changer mode (alternative to filament system simulation)
    bool tool_changer_mode_ = false; ///< Simulate tool changer instead of filament system

    // AFC mode (alternative to Happy Hare simulation)
    bool afc_mode_ = false;              ///< Simulate AFC Box Turtle instead of Happy Hare
    bool multi_unit_mode_ = false;       ///< Simulate multi-unit AFC (2x Box Turtle)
    bool mixed_topology_mode_ = false;   ///< Simulate mixed topology (BT + 2x OpenAMS)
    bool vivid_mixed_mode_ = false;      ///< Simulate 2x BoxTurtle + 1x ViViD
    bool ifs_mode_ = false;              ///< Simulate AD5X IFS (4 slots, LINEAR)
    bool htlf_toolchanger_mode_ = false; ///< Simulate HTLF + Toolchanger mixed topology
    bool torture_mode_ = false;          ///< Simulate 5 units / 16 lanes / 4 shared extruders
    bool snapmaker_mode_ = false; ///< Simulate Snapmaker U1 (4 slots, PARALLEL, non-editable)
    std::vector<PathTopology> unit_topologies_; ///< Per-unit topology storage

    // Endless spool simulation state
    bool endless_spool_supported_ = true; ///< Whether endless spool is supported
    bool endless_spool_editable_ = true;  ///< Whether config is editable (AFC) vs read-only (HH)

    // Device actions mock state
    std::vector<helix::printer::DeviceSection> mock_device_sections_;
    std::vector<helix::printer::DeviceAction> mock_device_actions_;
    std::string last_action_id_;
    std::any last_action_value_;

    // Gcode response injection (for simulating action:prompt from mock)
    std::function<void(const std::string&)> gcode_response_callback_;

    // Deferred state scenario (applied in start())
    std::string initial_state_scenario_;
    std::thread scenario_thread_; ///< Thread for deferred loading/bypass scenario
    std::atomic<bool> scenario_thread_running_{false}; ///< Guards against double-join
    bool mock_toolhead_unaccounted_ = false; ///< "unaccounted" scenario staged (gate input)

    // Test override for native-tracking capability. False in production; tests
    // flip this to exercise the FilamentConsumptionTracker gating path.
    bool tracks_consumption_natively_ = false;

    // Test override for tool->slot identity mapping. False in production;
    // Phase 5 routing tests set this true to simulate a tool-changer and
    // exercise per-extruder dispatch.
    bool identity_extruder_mapping_ = false;
};
