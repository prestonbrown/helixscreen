// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_subscription_backend.h"
#include "async_lifetime_guard.h"
#include "error_event.h"
#include "filament_slot_override_store.h"
#include "slot_registry.h"

#include <ctime>
#include <functional>
#include <map>
#include <optional>
#include <string>

/**
 * @file ams_backend_happy_hare.h
 * @brief Happy Hare MMU backend implementation
 *
 * Implements the AmsBackend interface for Happy Hare MMU systems.
 * Communicates with Moonraker to control the MMU via G-code commands
 * and receives state updates via printer.mmu.* subscriptions.
 *
 * Happy Hare Moonraker Variables:
 * - printer.mmu.gate       (int): Current gate (-1=none, -2=bypass)
 * - printer.mmu.tool       (int): Current tool
 * - printer.mmu.filament   (string): "Loaded" or "Unloaded"
 * - printer.mmu.action     (string): "Idle", "Loading", etc.
 * - printer.mmu.gate_status (array[int]): -1=unknown, 0=empty, 1=available, 2=from_buffer
 * - printer.mmu.gate_color_rgb (array[int]): RGB values like 0xFF0000
 * - printer.mmu.gate_material (array[string]): "PLA", "PETG", etc.
 *
 * G-code Commands:
 * - MMU_LOAD GATE={n}   - Load filament from specified gate
 * - MMU_UNLOAD          - Unload current filament
 * - MMU_SELECT GATE={n} - Select gate without loading
 * - T{n}                - Tool change (unload + load)
 * - MMU_HOME            - Home the selector
 * - MMU_RECOVER         - Attempt error recovery
 */
class AmsBackendHappyHare : public AmsSubscriptionBackend {
  public:
    /**
     * @brief Construct Happy Hare backend
     *
     * @param api Pointer to IMoonrakerAPI (for sending G-code commands)
     * @param client Pointer to helix::IMoonrakerClient (for subscribing to updates)
     *
     * @note Both pointers must remain valid for the lifetime of this backend.
     */
    AmsBackendHappyHare(IMoonrakerAPI* api, helix::IMoonrakerClient* client);
    ~AmsBackendHappyHare() override;

    /**
     * @brief Bare filament-sensor names Happy Hare owns (no AMS keyword).
     *
     * extruder, toolhead, filament_tension, filament_compression. The
     * keyword-bearing sensors (mmu_gate / mmu_pre_gate_N / mmu_gear_N) are
     * caught by PrinterHardware's substring path, not here. Static and
     * discovery-free; @p discovery is accepted for signature uniformity with
     * other backends. See AmsBackend::sensor_belongs_to_backend (#1054).
     */
    static bool owns_filament_sensor(const std::string& bare_name,
                                     const helix::PrinterDiscovery& discovery);

    // State queries
    [[nodiscard]] AmsSystemInfo get_system_info() const override;
    [[nodiscard]] AmsType get_type() const override;
    [[nodiscard]] const char* get_klipper_object_name() const override {
        return "mmu"; // Matches the Klipper object name
    }
    // Happy Hare reports printer.mmu.sync_feedback_bias; a value > -1.5 means real
    // bias data is available (the buffer meter, path-canvas tint, and clog buffer
    // page render proportional bias). -1.5 is the "no data" sentinel.
    [[nodiscard]] bool
    supports_sync_feedback_visualization(const AmsSystemInfo& info) const override {
        return info.sync_feedback_bias > -1.5f;
    }
    [[nodiscard]] bool manages_active_spool() const override;
    [[nodiscard]] SlotInfo get_slot_info(int slot_index) const override;

    // Path visualization
    [[nodiscard]] int get_bowden_progress() const override {
        return bowden_progress_;
    }
    [[nodiscard]] PathTopology get_topology() const override;
    [[nodiscard]] PathTopology get_unit_topology(int unit_index) const override;
    [[nodiscard]] PathSegment get_filament_segment() const override;
    [[nodiscard]] PathSegment get_slot_filament_segment(int slot_index) const override;
    [[nodiscard]] PathSegment infer_error_segment() const override;
    [[nodiscard]] bool slot_has_prep_sensor(int slot_index) const override;

  protected:
    // Operations. Gated by AmsSubscriptionBackend's NVI wrapper.
    // select_slot_moves_toolhead() stays false: MMU_SELECT drives the gate
    // selector and never touches the carriage.
    AmsError do_load_filament(int slot_index) override;
    AmsError do_unload_filament(int slot_index) override;
    AmsError do_select_slot(int slot_index) override;
    AmsError do_change_tool(int tool_number) override;

  public:
    // Recovery
    AmsError recover() override;
    AmsError reset() override;
    AmsError clear_fault(int slot_index) override;
    AmsError recover_with_state(const helix::RecoverStateRequest& request) override;
    [[nodiscard]] bool supports_recover_with_state() const override {
        return true;
    }
    /**
     * @brief The MMU_RECOVER line asserting @p request.
     *
     * Names only what the request knows: an unknown gate is omitted, and an
     * unset loaded flag leaves LOADED off so HH detects it with its sensors.
     * BYPASS=1 replaces GATE, which HH then forces to bypass.
     */
    [[nodiscard]] static std::string
    build_recover_command(const helix::RecoverStateRequest& request);
    AmsError eject_lane(int slot_index) override;
    [[nodiscard]] bool supports_lane_eject() const override {
        return true;
    }
    /**
     * @brief Move the selector to a gate without loading filament (MMU_SELECT).
     */
    AmsError select_gate(int slot_index) override;
    /**
     * @brief Jog the selector relative to the current gate (MMU_SELECT, clamped).
     */
    AmsError move_selector(int delta) override;
    [[nodiscard]] bool supports_gate_select() const override {
        return true;
    }
    [[nodiscard]] bool supports_gate_check() const override {
        return true;
    }
    [[nodiscard]] std::string reset_button_label() const override {
        return "Home";
    }
    /**
     * @brief Probe a single gate's sensor (MMU_CHECK_GATE GATE=n).
     */
    AmsError check_gate(int slot_index) override;
    /**
     * @brief Probe all gate sensors (MMU_CHECK_GATE, no params).
     */
    AmsError check_all_gates() override;
    AmsError cancel() override;

    // Error-center: classify a pausing MMU fault into a recovery ErrorEvent.
    [[nodiscard]] std::optional<helix::ErrorEvent>
    classify_error(const std::string& raw_line, const helix::ClassifyContext& ctx) const override;

    [[nodiscard]] std::vector<ToolchangePhase>
    toolchange_phase_template(StepOperationType op) const override;

    // Configuration
    AmsError set_slot_info(int slot_index, const SlotInfo& info, bool persist = true) override;
    void persist_slot_weight(int slot_index, float remaining_weight_g,
                             float total_weight_g) override;
    AmsError set_tool_mapping(int tool_number, int slot_index) override;

    // Bypass mode
    AmsError enable_bypass() override;
    AmsError disable_bypass() override;
    [[nodiscard]] bool is_bypass_active() const override;
    /// mmu.filament "Loaded" while mmu.gate names no gate (-1). Gate -2 is
    /// bypass — accounted there (the gate layer also silences under bypass).
    [[nodiscard]] std::optional<bool> toolhead_filament_unaccounted() const override;
    /// Happy Hare users have a console; the screen passes their command through
    /// rather than synthesising a prerequisite operation they never asked for
    /// (#1229).
    [[nodiscard]] bool allows_implicit_chaining() const override {
        return false;
    }

    // === Endless Spool ===
    //
    // Group-based and settable at RUNTIME - not a config-file read. `mmu.
    // endless_spool_groups` gives one group id per gate and `MMU_ENDLESS_SPOOL
    // GROUPS=<csv>` rewrites the whole array, so a write is a Group edit that
    // can move other gates' relations. Editing is refused on a multi-unit rig
    // because the G-code has no `UNIT=` and acts on the selected unit
    // (Happy-Hare extras/mmu/mmu.py cmd_MMU_ENDLESS_SPOOL).

    /// @note Takes `mutex_`; callers must NOT hold it.
    [[nodiscard]] helix::printer::EndlessSpoolCapabilities
    get_endless_spool_capabilities() const override;

    /// @note Takes `mutex_`; callers must NOT hold it.
    [[nodiscard]] helix::printer::EndlessSpoolConfig get_endless_spool_config() const override;

    /**
     * @brief Reset all tool mappings to defaults
     *
     * Resets tool-to-gate mappings to 1:1 (T0->Gate0, T1->Gate1, etc.)
     * by iterating through all tools and calling set_tool_mapping().
     *
     * @return AmsError with result
     */
    AmsError reset_tool_mappings() override;

    /**
     * @brief Restore the endless-spool groups to their config defaults.
     *
     * Overrides the base's clear-every-slot loop because Happy Hare has a real
     * primitive: `MMU_ENDLESS_SPOOL ENABLE=1 RESET=1 QUIET=1`.
     *
     * `ENABLE=1` is required and is NOT the silent side effect the edit path
     * had: `cmd_MMU_ENDLESS_SPOOL` early-returns before honouring `RESET` while
     * endless spool is disabled, and `_reset_endless_spool()` then assigns AND
     * persists `default_endless_spool_enabled`, so the momentary enable is
     * overwritten by the config default (mmu.py `_persist_endless_spool`).
     *
     * @note Holds no lock.
     */
    AmsError reset_endless_spool() override;

    // Dryer support (v4 - KMS/EMU hardware with heaters)
    [[nodiscard]] DryerInfo get_dryer_info(int unit = 0) const override;
    AmsError start_drying(float temp_c, int duration_min, int fan_pct = -1, int unit = 0) override;
    AmsError stop_drying(int unit = 0) override;
    [[nodiscard]] bool has_environment_sensors() const override {
        return true; // Live temp/target read from heater_generic via Moonraker subscriptions
    }

    /// Delete this gate's user override ("Clear Spool").
    void clear_slot_override(int slot_index) override;

    /// Publish the external spool as lane{N+1} in the SHARED lane_data
    /// namespace — Happy Hare's plugin never publishes its bypass/external
    /// spool (verified: push_lane_data iterates gates only), and its boot-time
    /// cleanup deletes records with lane >= num_gates. Our entry is wiped at
    /// HH boot; the AmsState event triggers (bypass engage, external-spool
    /// edit) re-publish.
    void publish_external_spool_lane(const SlotInfo* spool) override;

    [[nodiscard]] bool has_firmware_spool_persistence() const override {
        return true; // Happy Hare persists via MMU_GATE_MAP SPOOLID
    }

    [[nodiscard]] bool printer_reports_spool_ids() const override {
        return true; // Happy Hare publishes gate spool_id in mmu status
    }

    [[nodiscard]] RemapStrategy get_remap_strategy() const override {
        return RemapStrategy::Native;
    }

    /// Happy Hare owns the tool-to-gate map (MMU_TTG_MAP) and get_tool_mapping()
    /// returns it.
    [[nodiscard]] bool owns_tool_mapping_table() const override {
        return true;
    }

    // Tool Mapping support
    /**
     * @brief Get current tool-to-slot mapping
     *
     * Returns the tool_to_slot_map from system_info_ (populated from ttg_map).
     *
     * @return Vector where index=tool, value=slot
     */
    [[nodiscard]] std::vector<int> get_tool_mapping() const override;

    /// Happy Hare publishes the complete ttg_map in printer.mmu status, so a
    /// restore can be confirmed against firmware truth rather than our own
    /// optimistic write (#1270). Cleaner than a per-lane echo: the whole map
    /// lands in one update, so there is no partial-match intermediate state.
    [[nodiscard]] bool reports_firmware_tool_mapping() const override {
        return true;
    }

    [[nodiscard]] uint64_t firmware_tool_mapping_generation() const override;

    // NOTE: has_per_slot_loaded_authority() is deliberately NOT overridden.
    // printer.mmu.gate and printer.mmu.filament are Happy Hare's own values,
    // parsed verbatim from one object into the aggregate pair, so the aggregate
    // rule already answers with firmware truth here — unlike AFC, whose
    // current_slot we derive from several sources. gate_status carries fill
    // state, not seating, so the per-gate LOADED stamp is derived FROM that
    // aggregate; believing it back would add staleness and would drop the
    // highlight on a gate that ran out (gate_status 0) while its filament is
    // still at the toolhead (prestonbrown/helixscreen#1199).

    // Device Management
    [[nodiscard]] std::vector<helix::printer::DeviceSection> get_device_sections() const override;
    [[nodiscard]] std::vector<helix::printer::DeviceAction> get_device_actions() const override;
    AmsError execute_device_action(const std::string& action_id,
                                   const std::any& value = {}) override;

  protected:
    /**
     * @brief Transport for one endless-spool edge: `MMU_ENDLESS_SPOOL GROUPS=`.
     *
     * Rebuilds the whole gate->group array and joins @p slot_index to
     * @p backup_slot's group (or moves it to a fresh standalone group when
     * @p backup_slot is -1).
     *
     * Refuses when endless spool is switched OFF instead of silently switching
     * it on. `cmd_MMU_ENDLESS_SPOOL` ignores `GROUPS` while disabled, so the old
     * unconditional `ENABLE=1` was the only thing making the write land - and it
     * turned the feature on as a side effect of editing one backup, persisting
     * that through `mmu_state_enable_endless_spool`. Enabling is a separate
     * decision the user has to make.
     *
     * @note Takes `mutex_` to build the CSV, releases it before the G-code send.
     */
    AmsError apply_endless_spool_backup(int slot_index, int backup_slot) override;

    // Allow test helper access to private members
    friend class AmsBackendHappyHareTestHelper;
    friend class AmsBackendHappyHareEndlessSpoolHelper;
    friend class AmsBackendHHMultiUnitHelper;
    friend class HappyHareErrorStateHelper;
    friend class HappyHareCharHelper;
    friend class HHToolchangeTestHelper;
    friend class HhFaultEventCharHelper;
    friend class HappyHareTestAccess;

    // --- AmsSubscriptionBackend hooks ---
    void on_started() override;
    void handle_status_update(const nlohmann::json& notification) override;
    const char* backend_log_tag() const override {
        return "[AMS HappyHare]";
    }

  private:
    // === User-attached slot identity (FilamentSlotOverrideStore) =============
    //
    // Happy Hare's gate map carries spool_id / material / colour, but not brand,
    // spool_name, total_weight_g, colour name or the Spoolman filament+vendor
    // ids. Those live only here.
    //
    // PRIVATE namespace: lane_data belongs to the Happy Hare plugin, same as
    // AFC. See AmsBackendAfc for the full rationale.
    //
    // Written blind — no Happy Hare hardware on hand; mirrors AFC exactly.
    static constexpr const char* OVERRIDE_NAMESPACE = "helix-screen-hh-overrides";
    std::unique_ptr<helix::ams::FilamentSlotOverrideStore> override_store_;
    /// Store on the SHARED lane_data namespace, used only by
    /// publish_external_spool_lane. Happy Hare's plugin owns that namespace.
    std::unique_ptr<helix::ams::FilamentSlotOverrideStore> lane_publish_store_;
    std::unordered_map<int, helix::ams::FilamentSlotOverride> overrides_;
    void apply_overrides(SlotInfo& slot, int slot_index);
    void persist_override(int slot_index, const SlotInfo& info);

    // Build context-aware recovery actions from live MMU state. Caller holds mutex_
    // (the base declares that contract; mutex_ is non-recursive, so this must not
    // lock).
    [[nodiscard]] std::vector<helix::RecoveryAction> build_recovery_actions() const override;

    // Synthesize a toolchange step index from the current AmsAction and push it
    // to AmsState's step subject (deferred to the main thread). Happy Hare emits
    // no // narration, so the backend drives the bar itself. Caller holds mutex_.
    void sync_narration_step();

    /**
     * @brief Parse MMU state from Moonraker JSON
     *
     * Extracts mmu object from notification and updates system_info_.
     *
     * @param mmu_data JSON object containing printer.mmu data
     */
    void parse_mmu_state(const nlohmann::json& mmu_data);

    /**
     * @brief Re-derive every gate's SlotStatus from gate_status + the loaded gate
     *
     * printer.mmu arrives as a delta: `gate_status`, `gate` and `filament` each
     * turn up in whatever frame changed them, and a toolchange typically carries
     * the latter two alone. Deriving the LOADED stamp inside the gate_status
     * branch therefore pinned it to whichever gate was loaded the last time a
     * gate's fill state happened to change (#1199). Called at the end of every
     * parse_mmu_state() instead, off the cached gate_status_raw_.
     *
     * Caller must hold mutex_.
     */
    void refresh_gate_statuses_locked();

    /**
     * @brief Initialize slot structures based on gate_status array size
     *
     * Called when we first receive gate_status to create the correct
     * number of SlotInfo entries.
     *
     * @param gate_count Number of gates detected
     */
    void initialize_slots(int gate_count);

    /**
     * @brief Validate gate index is within range
     *
     * @param gate_index Slot index to validate
     * @return AmsError (SUCCESS if valid, INVALID_GATE otherwise)
     */
    AmsError validate_slot_index(int gate_index) const;

    /**
     * @brief Query configfile.settings.mmu to determine tip method
     *
     * Reads form_tip_macro from Happy Hare config via Moonraker.
     * If macro name contains "cut", sets TipMethod::CUT (e.g., _MMU_CUT_TIP).
     * Otherwise sets TipMethod::TIP_FORM (e.g., _MMU_FORM_TIP).
     * Called once during start().
     */
    void query_tip_method_from_config();

    /**
     * @brief Query configfile.settings.mmu_machine to determine selector type
     *
     * Reads selector_type from Happy Hare config via Moonraker.
     * VirtualSelector = Type B (HUB topology), all others = Type A (LINEAR).
     * Called once during on_started().
     */
    void query_selector_type_from_config();

    /**
     * @brief Query configfile.settings.mmu_machine + mmu for heater name + max_temp
     *
     * Reads filament_heater from [mmu_machine] and heater_max_temp from [mmu].
     * Called once during on_started().
     */
    void query_heater_config_from_config();

    /**
     * @brief Parse heater config settings into dryer_info_
     *
     * Factored out of query_heater_config_from_config() for testability.
     * @param settings The configfile.settings JSON object
     * @param live_mmu_machine The live mmu_machine status object. Happy Hare v4
     *        publishes filament_heater / environment_sensor there, per unit,
     *        and leaves configfile carrying only the version; v3 has them in
     *        settings and passes an empty object here.
     */
    void apply_heater_config(const nlohmann::json& settings,
                             const nlohmann::json& live_mmu_machine = nlohmann::json::object());

    /**
     * @brief Parse live heater_generic temperature/target from a status update
     *
     * Scans the params object for the key matching filament_heater_name_ and
     * updates dryer_info_.current_temp_c / target_temp_c.
     * @param params The top-level params object from notify_status_update
     * @return true if the filament-heater key was present and parsed
     */
    bool apply_filament_heater_status(const nlohmann::json& params);

    /**
     * @brief Parse box humidity from an environment-sensor status update
     *
     * Mirrors Happy Hare's mmu_environment_manager._get_environment_status():
     * derives the bare sensor name from environment_sensor_name_ and reads the
     * "humidity" field from the env-sensor object or a matching humidity chip
     * (bme280/htu21d/sht3x/aht10) present in the params object.
     * @param params The top-level params object from notify_status_update
     * @return true if a humidity value was found and stored
     */
    bool apply_environment_sensor_status(const nlohmann::json& params);

    /**
     * @brief Check if this is a Type B MMU (hub topology)
     * @return true if selector_type is VirtualSelector
     */
    [[nodiscard]] bool is_type_b() const;

    /**
     * @brief Update topology on all existing units after selector_type is known
     */
    void update_unit_topologies();

    std::string selector_type_; ///< Selector type from config (e.g., "VirtualSelector" for Type B)

  public:
    /// What the connect-time `mmu_machine` / `configfile.settings` pair says
    /// about the install. Happy Hare 3 keeps every tunable on `[mmu]` and its
    /// version as a number there (3.42); Happy Hare 4 has no `[mmu]`, splits
    /// the tunables across `[mmu_parameters]`, `[mmu_unit_parameters <unit>]`
    /// and `[mmu_toolhead <name>]`, and publishes `happy_hare_version` on
    /// `mmu_machine`.
    /// One unit's fields from the live mmu_machine.unit_N (v4).
    struct MachineUnit {
        std::string selector_type;
        int first_gate = -1;
        int num_gates = 0;
        bool filament_always_gripped = false;
        std::optional<bool> filament_buffer;
        /// From the unit's configfile [mmu_unit <name>] encoder
        std::optional<bool> has_encoder;
        bool has_heater = false; ///< filament_heater or any filament_heaters entry
    };
    /// A per-unit capability v4 checks before it accepts a command or a
    /// MMU_TEST_CONFIG parameter.
    enum class UnitFeature { Servo, SelectorSpeed, Encoder, SyncToExtruder, FilamentBuffer };
    /// Whether @p unit has @p feature. v3 knows only Type A from Type B, and
    /// checks only parameter names, so there only the selector features apply.
    [[nodiscard]] static bool unit_supports(const MachineUnit& unit, UnitFeature feature, bool v4);

    struct MachineLayout {
        std::string version;             ///< happy_hare_version; empty when unknown
        double version_number = 0;       ///< 3.42, 4.0; 0 when unknown
        bool v4 = false;                 ///< the one v4 predicate
        int num_units = 1;               ///< mmu_machine.num_units, v4 only
        std::string unit_params_section; ///< "mmu_unit_parameters <unit 0>", v4 only
        std::string toolhead_section;    ///< "mmu_toolhead <name>", v4 only
        std::optional<bool> has_bypass;  ///< any unit's has_bypass, v4 only
        std::vector<MachineUnit> units;  ///< every mmu_machine.unit_N, v4 only
    };
    /// @p settings configfile.settings, @p live_mmu_machine the live object
    /// (either may be empty).
    [[nodiscard]] static MachineLayout read_machine_layout(const nlohmann::json& settings,
                                                           const nlohmann::json& live_mmu_machine);
    /// The name the install @p layout describes takes for tunable @p key,
    /// which callers spell the way HelixScreen does; empty when that version
    /// has none. Clog detection is ENABLE_CLOG_DETECTION /
    /// MMU_CALIBRATION_CLOG_LENGTH before 3.42 and FLOWGUARD_ENCODER_MODE /
    /// FLOWGUARD_ENCODER_MAX_MOTION from 3.42 on.
    [[nodiscard]] static std::string param_name(const std::string& key,
                                                const MachineLayout& layout);
    /// The configfile value of tunable @p key from whichever section @p layout
    /// keeps it in ([mmu] on v3; [mmu_parameters], the first unit's
    /// [mmu_unit_parameters] or its [mmu_toolhead] on v4), or nullptr.
    [[nodiscard]] static const nlohmann::json* find_config_param(const nlohmann::json& settings,
                                                                 const MachineLayout& layout,
                                                                 const std::string& key);

    /// MMU_TEST_CONFIG with the clog mode and, in manual mode, the length,
    /// named as the installed version takes them.
    [[nodiscard]] std::optional<std::string>
    clog_detection_mode_gcode(int mode, float det_length) const override;
    /// flowguard_encoder_max_motion from 3.42 on; nullopt before, where the
    /// length is the calibrated one the encoder reports live.
    [[nodiscard]] std::optional<float> clog_detection_length_setting() const override;

  private:
    /// Version and config layout. Caller holds mutex_.
    MachineLayout machine_layout_;
    /// MMU_TEST_CONFIG's parameter for @p key, uppercased; empty when the
    /// installed version has none. Caller holds mutex_.
    [[nodiscard]] std::string test_config_param_locked(const std::string& key) const;
    /// Unit @p unit has @p feature, from its mmu_machine fields, else the
    /// machine-wide selector type. Caller holds mutex_.
    [[nodiscard]] bool unit_supports_locked(int unit, UnitFeature feature) const;
    /// The selected unit when it has @p feature, else the first that does.
    /// Caller holds mutex_.
    [[nodiscard]] std::optional<int> unit_with_locked(UnitFeature feature) const;
    /// The unit an MMU_TEST_CONFIG of @p key targets: the selected unit when it
    /// takes the parameter, else the first unit that does; nullopt when none
    /// does or the installed version has no such parameter. v3 checks only
    /// names, so there only selector_move_speed depends on the unit.
    /// Caller holds mutex_.
    [[nodiscard]] std::optional<int> test_config_unit_locked(const std::string& key) const;
    /// `MMU_TEST_CONFIG <param>=<value>` for @p key, or nullopt when no unit
    /// takes it. Caller holds mutex_.
    [[nodiscard]] std::optional<std::string>
    test_config_command_locked(const std::string& key, const std::string& value) const;
    /// The selected unit (printer.mmu.unit). Caller holds mutex_.
    [[nodiscard]] int active_unit_locked() const;
    /// UNIT value naming every unit; v4 takes UNIT=ALL on its per-unit commands.
    static constexpr int kAllUnits = -1;
    /// " UNIT=<n>" (" UNIT=ALL" for kAllUnits) when a command has to name its
    /// unit: a v4 install with more than one unit refuses a per-unit command
    /// without it. Empty otherwise, and always on v3, whose MMU_TEST_CONFIG
    /// rejects an unknown parameter. Caller holds mutex_.
    [[nodiscard]] std::string unit_suffix_locked(int unit) const;
    /// The MMU_HEATER target for @p unit: UNIT= and the unit's gates on a
    /// multi-unit v4, else GATES= on a multi-unit rig. Caller holds mutex_.
    [[nodiscard]] std::string heater_suffix_locked(int unit) const;
    /// One heater target per command: @p unit's, or for the whole machine on a
    /// multi-unit v4, one per unit that has a heater. Locks mutex_.
    [[nodiscard]] std::vector<std::string> heater_targets_for_unit(int unit) const;
    /// @p command once per heater target; a failed drying start stops the
    /// units already started.
    AmsError send_heater_command(const std::string& command, int unit);
    /// Toolhead / extruder-entry sensor fitted and enabled, from
    /// printer.mmu.sensors; nullopt until a frame carries the dict. v4 refuses
    /// the toolhead distance tuned against a sensor that is not.
    std::optional<bool> toolhead_sensor_fitted_;
    std::optional<bool> extruder_sensor_fitted_;
    /// Store @p layout and the version it names. Caller holds mutex_.
    void set_machine_layout_locked(const MachineLayout& layout);

    // Async callback safety guard
    helix::AsyncLifetimeGuard lifetime_;

    // Cached MMU state
    helix::printer::SlotRegistry slots_;    ///< Single source of truth for per-slot state
    int num_units_{1};                      ///< Number of physical units (default 1)
    std::vector<int> per_unit_gate_counts_; ///< Per-unit gate counts for dissimilar multi-MMU (v4)
    int active_unit_{0};                    ///< Currently active MMU unit (v4)

    /// Whether printer.mmu.has_bypass has been observed at least once, so the
    /// resolved value gets logged even when it matches our optimistic default.
    bool bypass_support_seen_{false};
    /// Last printer.mmu.has_bypass; the bypass source on v3 only.
    std::optional<bool> status_has_bypass_;
    /// supports_bypass from whichever source the layout trusts. Caller holds mutex_.
    void apply_bypass_support_locked();

    /// Last printer.mmu.gate_status array, raw Happy Hare values (-1 unknown,
    /// 0 empty, 1 available, 2 from_buffer). Kept because the array and the
    /// gate/filament pair arrive in independent deltas and
    /// refresh_gate_statuses_locked() needs both to derive a status.
    std::vector<int> gate_status_raw_;

    // Path visualization state
    int filament_pos_{0};     ///< Happy Hare filament_pos value
    int bowden_progress_{-1}; ///< Bowden loading progress 0-100% (-1=unavailable, v4)
    PathSegment error_segment_{PathSegment::NONE}; ///< Inferred error location

    // Dryer state (v4 - KMS/EMU hardware)
    DryerInfo dryer_info_;
    std::time_t dry_end_epoch_ = 0; ///< HelixScreen-initiated drying end (epoch s), 0 = none
    std::function<std::time_t()> now_fn_ = [] { return std::time(nullptr); };
    // Dryer enclosure heaters + environment sensors. Happy Hare supports either a
    // single shared enclosure (scalar filament_heater / environment_sensor) OR a
    // per-gate setup (plural filament_heaters / environment_sensors, one entry per
    // gate, distributed across units for multi-MMU). See [mmu_machine] config and
    // mmu_environment_manager.py. The scalar name (when set) drives the global
    // dryer model (dryer_info_); the maps hold live readings keyed by object name
    // for per-unit resolution in get_system_info().
    std::string filament_heater_name_;    ///< scalar [mmu_machine] filament_heater (primary heater)
    std::string environment_sensor_name_; ///< scalar [mmu_machine] environment_sensor
    std::vector<std::string> filament_heaters_; ///< per-gate heaters (plural), empty if shared
    std::vector<std::string>
        environment_sensors_;                  ///< per-gate env sensors (plural), empty if shared
    std::map<std::string, float> heater_temp_; ///< live temp (°C) keyed by heater object name
    std::map<std::string, float>
        sensor_humidity_; ///< live humidity (%RH) keyed by env-sensor object name
    std::map<std::string, float>
        sensor_temp_; ///< live ambient temp (°C) keyed by env-sensor object name

    // Error state tracking
    std::string reason_for_pause_; ///< Last reason_for_pause from MMU (descriptive error text)

    // --- Config defaults from configfile.settings.mmu ---

    /// Cached config defaults parsed from configfile.settings.mmu
    struct ConfigDefaults {
        float gear_from_buffer_speed = 150.0f;
        float gear_from_spool_speed = 60.0f;
        float gear_unload_speed = 80.0f;
        float selector_move_speed = 200.0f;
        float extruder_load_speed = 45.0f;
        float extruder_unload_speed = 45.0f;
        float toolhead_sensor_to_nozzle = 62.0f;
        float toolhead_extruder_to_nozzle = 72.0f;
        float toolhead_entry_to_extruder = 0.0f;
        float toolhead_ooze_reduction = 2.0f;
        int sync_to_extruder = 0;
        int clog_detection = 0;
        /// Manual-mode clog detection length, when the config names one
        std::optional<float> detection_length;
        bool loaded = false;
    };
    ConfigDefaults config_defaults_;

    /// User overrides (set via UI, persisted to Config)
    struct UserOverrides {
        std::optional<float> gear_from_buffer_speed;
        std::optional<float> gear_from_spool_speed;
        std::optional<float> gear_unload_speed;
        std::optional<float> selector_move_speed;
        std::optional<float> extruder_load_speed;
        std::optional<float> extruder_unload_speed;
        std::optional<float> toolhead_sensor_to_nozzle;
        std::optional<float> toolhead_extruder_to_nozzle;
        std::optional<float> toolhead_entry_to_extruder;
        std::optional<float> toolhead_ooze_reduction;
        std::optional<int> sync_to_extruder;
        std::optional<int> clog_detection;
    };
    UserOverrides user_overrides_;

    // Status-backed values (from printer.mmu.* subscriptions)
    std::string led_exit_effect_;
    std::string espooler_active_;
    int flowguard_encoder_mode_ = -1; ///< -1 = not yet received from Moonraker

    void query_config_defaults();
    /// Read the layout and the speed/distance defaults from the connect-time
    /// configfile and live mmu_machine, then re-apply persisted overrides.
    void apply_config_defaults(const nlohmann::json& settings,
                               const nlohmann::json& live_mmu_machine);
    void load_persisted_overrides();
    void save_override(const std::string& key, float value);
    void save_override(const std::string& key, int value);
    void reapply_overrides();

    /// Get the config default float for a given action key
    [[nodiscard]] float get_config_default_float(const std::string& key) const;
    /// Get the config default int for a given action key
    [[nodiscard]] int get_config_default_int(const std::string& key) const;
};
