// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "afc_config_manager.h"
#include "ams_subscription_backend.h"
#include "async_lifetime_guard.h"
#include "error_event.h"
#include "filament_slot_override_store.h"
#include "slot_registry.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>

/**
 * @file ams_backend_afc.h
 * @brief AFC-Klipper-Add-On backend implementation
 *
 * Implements the AmsBackend interface for AFC (Armored Turtle / Box Turtle)
 * multi-filament systems. Communicates with Moonraker to control AFC via
 * G-code commands and receives state updates via printer.afc.* subscriptions
 * and database lane_data queries.
 *
 * AFC Terminology Differences from Happy Hare:
 * - "Lanes" instead of "Gates"
 * - "Units" are typically called "Box Turtles" or "AFC units"
 * - Lane names may be configurable (lane1, lane2... or custom names)
 *
 * AFC State Sources:
 * - Printer object: printer.afc with status info
 * - Moonraker database: lane_data (via server.database.get_item)
 *
 * Lane Data Structure (from database):
 * {
 *   "lane1": {"color": "FF0000", "material": "PLA", "loaded": false},
 *   "lane2": {"color": "00FF00", "material": "PETG", "loaded": true}
 * }
 *
 * G-code Commands:
 * - CHANGE_TOOL LANE={name} - Load/change filament from specified lane
 * - TOOL_UNLOAD             - Unload current filament
 * - SET_MAP LANE={name} MAP=T{n} - Map lane to tool number
 * - AFC_RESET               - Reset/re-prep all lanes
 * - T{n}                   - Tool change (unload + load)
 */
/**
 * @brief Per-extruder info for toolchanger configurations
 *
 * When AFC detects a toolchanger (num_extruders > 1), the webhook status
 * includes per-extruder data: which lane is loaded and which lanes can
 * feed each extruder.
 */
struct AfcExtruderInfo {
    std::string name;                         ///< Extruder name ("extruder", "extruder1")
    std::string lane_loaded;                  ///< Currently loaded lane (or empty)
    std::vector<std::string> available_lanes; ///< Lanes that can feed this extruder
    float tool_stn = 72.0f;                   ///< Sensor-to-nozzle distance (mm)
    float tool_stn_unload = 100.0f;           ///< Unload retraction distance (mm)
    float tool_sensor_after_extruder = 0.0f;  ///< Post-sensor clear distance (mm)
};

/**
 * @brief Per-tool toolchanger state from the AFC_extruder Klipper object
 *
 * AFC v1.2.0 (#768) added these so UIs can show which toolhead is being docked
 * versus picked up during a swap. Absent on older AFC, so the defaults must read
 * as "nothing special happening".
 *
 * Kept in a name-keyed map rather than on AfcExtruderInfo because that vector is
 * indexed POSITIONALLY as a tool number and is rebuilt from AFC.system.extruders
 * on every status update.
 */
struct AfcToolState {
    std::string status;         ///< Per-tool AFC State ("ToolDock", "ToolPickup", "Idle", …)
    bool next_pickup = false;   ///< True on the tool about to be picked up
    bool is_standalone = false; ///< Standalone toolhead (own lane) vs lane-fed
};

/**
 * @brief Toolhead sensor state for one AFC_extruder, with the lane that owns it
 *
 * tool_start / tool_end are the extruder's own filament switches, so they say
 * "something is at this toolhead" without saying whose. lane_loaded is AFC's
 * answer to that second question: set_loaded() assigns tool_loaded, afc.current
 * and extruder_obj.lane_loaded together, and set_unloaded() clears them
 * together, so the pairing is atomic upstream.
 *
 * Kept per extruder rather than folded into the single tool_start_sensor_ /
 * tool_end_sensor_ pair because a toolchanger has one set per toolhead, and
 * attribution is only meaningful alongside the sensors it attributes.
 */
struct AfcExtruderSensors {
    bool tool_start = false; ///< Toolhead entry sensor (tool_start_status)
    bool tool_end = false;   ///< Toolhead exit/nozzle sensor (tool_end_status)
    std::string lane_loaded; ///< Lane seated at this extruder; empty when none

    /// Whether this toolhead is on the carriage right now.
    ///
    /// AFC's own answer to "what is mounted", published per extruder. Preferred
    /// over Klipper's `toolchanger` object for two reasons: it is plural, so it
    /// can express an IDEX machine carrying two toolheads at once (#1201), and it
    /// does not depend on the Klipper toolchanger modules that AFC intends to
    /// absorb. Nothing read this field before #1229.
    bool on_shuttle = false;
    /// Whether AFC ever reported on_shuttle for this extruder. Older AFC omits
    /// it entirely, and "absent" must not be read as "not mounted".
    bool has_on_shuttle = false;
};

/**
 * @brief Per-unit info parsed from flat string units and unit-level Klipper objects
 *
 * When AFC reports units as flat strings (e.g., "OpenAMS AMS_1", "Box_Turtle Turtle_1"),
 * this struct stores the parsed type/name and the Klipper object key used to receive
 * unit-level status updates (e.g., "AFC_OpenAMS AMS_1"). The lanes, extruders, hubs,
 * and buffers arrays are populated from the unit-level Klipper object data.
 */
struct AfcUnitInfo {
    std::string klipper_key; ///< Klipper object key (e.g., "AFC_BoxTurtle Turtle_1")
    std::string name;        ///< Unit instance name (e.g., "Turtle_1", "AMS_1")
    std::string type;        ///< Unit type (e.g., "Box_Turtle", "OpenAMS")

    std::vector<std::string> lanes;     ///< Lane names belonging to this unit
    std::vector<std::string> extruders; ///< Extruder names for this unit
    std::vector<std::string> hubs;      ///< Hub names for this unit
    std::vector<std::string> buffers;   ///< Buffer names for this unit

    PathTopology topology = PathTopology::HUB; ///< Derived topology for this unit

    /// Per-lane hub routing. Parallel to `lanes` vector.
    /// true = lane routes through hub, false = direct to extruder.
    std::vector<bool> lane_is_hub_routed;
};

class AmsBackendAfc : public AmsSubscriptionBackend {
  public:
    /**
     * @brief Construct AFC backend
     *
     * @param api Pointer to IMoonrakerAPI (for sending G-code commands)
     * @param client Pointer to helix::IMoonrakerClient (for subscribing to updates)
     *
     * @note Both pointers must remain valid for the lifetime of this backend.
     */
    AmsBackendAfc(IMoonrakerAPI* api, helix::IMoonrakerClient* client);
    ~AmsBackendAfc() override;

    /**
     * @brief Bare filament-sensor names AFC owns (no AMS keyword).
     *
     * Fixed extruder sensors tool_start / tool_end; per-lane sensors
     * <lane>_prep / <lane>_load / <lane>_selector for every discovered lane;
     * per-buffer sensors <buffer>_expanded / <buffer>_compressed for every
     * discovered buffer; and the HTLF <unit>_home_pin suffix (any unit name).
     * Reads lane/buffer names from @p discovery. Static; see
     * AmsBackend::sensor_belongs_to_backend (#1054).
     */
    static bool owns_filament_sensor(const std::string& bare_name,
                                     const helix::PrinterDiscovery& discovery);

    // State queries
    [[nodiscard]] AmsSystemInfo get_system_info() const override;
    [[nodiscard]] AmsType get_type() const override;
    [[nodiscard]] bool is_afc_system() const override {
        return true;
    }
    /// AFC users have a console; the screen passes their command through rather
    /// than synthesising a prerequisite operation they never asked for (#1229).
    [[nodiscard]] bool allows_implicit_chaining() const override {
        return false;
    }
    [[nodiscard]] const char* get_klipper_object_name() const override {
        return "AFC"; // Matches the Klipper object name (uppercase)
    }
    AmsError clear_message_queue() override;
    [[nodiscard]] bool manages_active_spool() const override {
        return true;
    }
    [[nodiscard]] bool tracks_weight_locally() const override {
        return true;
    }
    [[nodiscard]] SlotInfo get_slot_info(int slot_index) const override;

    /**
     * @brief Does this lane status payload prove AFC publishes the v1.2.0 field set?
     *
     * Feature detection, deliberately NOT a version comparison. AFC has no
     * trustworthy version signal: the `afc-install` DB namespace has been an
     * orphan since their 7d20db7, `AFC_VERSION` is a hand-bumped literal that sat
     * at 1.1.37 through the whole v1.2.0 release, and v1.2.0's own get_status()
     * publishes no version key at all (upstream #807 is still an open PR). A live
     * BoxTurtle reported "1.0.0" while running v1.1.0.
     *
     * `filament_name`, `multi_color_hexes` and `initial_weight` are emitted
     * together from one `if not save_to_file:` block in AFC_lane.get_status(), so
     * any of them proves the whole block. Verified on one physical BoxTurtle
     * across an upgrade: all three absent on v1.1.0, all three present on v1.2.0.
     *
     * @warning Only meaningful on a COMPLETE status object — the subscription's
     * first baseline frame. Every later frame is a delta where an absent key
     * means "unchanged", not "unsupported".
     */
    [[nodiscard]] static bool status_has_modern_fields(const nlohmann::json& lane_status);

    // Path visualization
    [[nodiscard]] PathTopology get_topology() const override;
    [[nodiscard]] PathTopology get_unit_topology(int unit_index) const override;
    [[nodiscard]] PathSegment get_filament_segment() const override;
    [[nodiscard]] PathSegment get_slot_filament_segment(int slot_index) const override;
    [[nodiscard]] PathSegment infer_error_segment() const override;
    [[nodiscard]] bool slot_has_prep_sensor(int slot_index) const override;

    /// AFC reports load state per lane (AFC_stepper.<lane>.tool_loaded), which
    /// parse_afc_stepper() turns into SlotStatus::LOADED. That beats deriving a
    /// per-lane answer from the aggregate current_slot pointer, which we resolve
    /// from several sources and which goes null mid-toolchange (#1194).
    [[nodiscard]] bool has_per_slot_loaded_authority() const override {
        return true;
    }

    /// True when the extruder that names this lane as loaded has filament at
    /// either of its sensors. AFC_extruder carries tool_start_status /
    /// tool_end_status plus the lane_loaded that owns them, so the signal is
    /// real and attributable — an unattributed trip stays false rather than
    /// being blamed on an arbitrary lane.
    [[nodiscard]] bool slot_has_filament_at_toolhead(int slot_index) const override;

    /// Filament at a toolhead sensor while no lane claims the toolhead —
    /// neither AFC.current (toolhead_lane_) nor any lane's persisted
    /// tool_loaded. See toolhead_is_free_unlocked() for why these three
    /// signals and NOT system_info_.filament_loaded.
    [[nodiscard]] std::optional<bool> toolhead_filament_unaccounted() const override;

    /// Status-driven fault, surfaced by AmsErrorBridge on the AmsAction::ERROR
    /// edge. Keyed on AFC's error_state, which upstream sets in lockstep with
    /// current_state = State.ERROR — the same transition that produced the edge.
    /// Complements classify_error(): the `!!` line arrives BEFORE AFC pauses, so
    /// the classifier's paused catch-all misses the fault that this catches when
    /// the status update lands (prestonbrown/helixscreen#1171).
    [[nodiscard]] std::optional<helix::ErrorEvent> current_error() const override;

    /// L1: recognize AFC toolhead jam / lane / hub faults and emit a CRITICAL
    /// ErrorEvent with context-aware recovery actions. Falls back to a catch-all
    /// for any pausing !! while error_state_ is set. Returns nullopt otherwise so
    /// the generic classifier handles the line.
    [[nodiscard]] std::optional<helix::ErrorEvent>
    classify_error(const std::string& raw_line, const helix::ClassifyContext& ctx) const override;

    /// L2 (S1/S2): ordered toolchange phase templates and narration→phase-id
    /// matcher for AFC. AFC emits `//` narration lines for feed/purge/brush/
    /// clean/cut/poop/kick; the sidebar step bar uses these to label phases
    /// correctly (S1) and to surface brush/clean/cut/poop/kick steps (S2).
    [[nodiscard]] std::vector<ToolchangePhase>
    toolchange_phase_template(StepOperationType op) const override;
    [[nodiscard]] std::optional<std::string>
    match_narration_phase(const std::string& narration) const override;

    /// AFC emits its load/unload narration with NO `//` prefix (`Loading lane3`,
    /// `lane3 is now loaded in toolhead t:0`, ...), so the semantically important
    /// half of the step model arrives on the bare console channel. Matched by
    /// anchored shape rather than substring — see the .cpp for the shape table
    /// and docs/devel/FILAMENT_MANAGEMENT.md § "AFC console response contract".
    [[nodiscard]] std::optional<std::string>
    match_bare_narration_phase(const std::string& line) const override;

    /// A line naming AFC or a lane that matched no phase, minus AFC's known
    /// phase-less lines (tool-change banners, `already loaded`, buffer
    /// bookkeeping), which would otherwise log on every single toolchange.
    [[nodiscard]] bool is_narration_drift_candidate(const std::string& line) const override;

  protected:
    // Operations. Gated by AmsSubscriptionBackend's NVI wrapper.
    // select_slot_moves_toolhead() stays false: an AFC select positions a lane,
    // it does not drive the toolhead.
    AmsError do_load_filament(int slot_index) override;
    AmsError do_unload_filament(int slot_index) override;
    AmsError do_select_slot(int slot_index) override;
    AmsError do_change_tool(int tool_number) override;

  public:
    // Recovery
    AmsError recover() override;
    AmsError reset() override;
    AmsError clear_fault(int slot_index) override;
    /// AFC_LANE_RESET retracts from the bowden to the hub. Needs the lane routed
    /// to a hub, that hub's sensor triggered, a free toolhead, the lane's own
    /// load switch triggered, and AFC naming this exact lane as active. See
    /// can_recover_lane_position() for why each is load-bearing.
    [[nodiscard]] bool can_recover_lane_position(int slot_index) const override;
    AmsError recover_lane_position(int slot_index) override;
    /// True when AFC names a still-existing lane as active (AFC.current_lane /
    /// current_load). The BoxTurtle hub sensor is shared across every lane on
    /// the unit, so an unattributed trigger cannot say whose filament caused it
    /// — and can_recover_lane_position() now offers nothing at all in that case,
    /// so this is implied by it rather than merely ranking it.
    [[nodiscard]] bool lane_recovery_is_attributed() const override;

    /// Delete this slot's user override ("Clear Spool"). AFC previously
    /// inherited the no-op default, so the button did nothing here.
    void clear_slot_override(int slot_index) override;

    /// Publish the external spool as T{N} (one past the last lane) in the
    /// SHARED lane_data namespace — AFC's own plugin never publishes its
    /// extern (verified: AFC_lane.send_lane_data runs only for lanes with a
    /// tool mapping, and AFC deletes the whole namespace at boot). Our entry
    /// is wiped by that boot delete; the AmsState event triggers (bypass
    /// engage, external-spool edit) re-publish.
    void publish_external_spool_lane(const SlotInfo* spool) override;
    AmsError eject_lane(int slot_index) override;
    [[nodiscard]] bool supports_lane_eject() const override {
        return true;
    }

    /// AFC refuses LANE_UNLOAD outright while printing, so the affordance must
    /// grey out with the rest — see AmsBackend for why this is not the default.
    [[nodiscard]] bool cold_lane_ops_refused_during_print() const override {
        return true;
    }
    AmsError cancel() override;

    // Configuration
    AmsError set_slot_info(int slot_index, const SlotInfo& info, bool persist = true) override;
    AmsError set_tool_mapping(int tool_number, int slot_index) override;

    // Bypass mode
    AmsError enable_bypass() override;
    AmsError disable_bypass() override;
    [[nodiscard]] bool is_bypass_active() const override;

    // Capability queries
    /**
     * @brief AFC automatically heats extruder using default_material_temps
     * @return true - AFC handles preheat via its configuration
     */
    [[nodiscard]] bool supports_auto_heat_on_load() const override {
        return true;
    }

    [[nodiscard]] RemapStrategy get_remap_strategy() const override {
        return RemapStrategy::Native;
    }

    [[nodiscard]] bool has_firmware_spool_persistence() const override {
        return true; // AFC uses SET_SPOOL_ID gcode for persistence
    }

    [[nodiscard]] bool printer_reports_spool_ids() const override {
        return true; // AFC publishes a lane spool_id in its status
    }

    /// Per-lane remember_spool = true on EVERY reporting lane (ALL
    /// semantics — a mixed config still leaves the toggle governing the
    /// false lanes). See AmsBackend::printer_retains_spool_info().
    [[nodiscard]] bool printer_retains_spool_info() const override;

    /// [AFC] auto_home from AFC.cfg — true only once afc_config_ has landed
    /// (see AmsBackend::delegates_homing_to_printer()).
    [[nodiscard]] bool delegates_homing_to_printer() const override {
        return afc_config_ && afc_config_->is_loaded() &&
               afc_config_->parser().get_bool("AFC", "auto_home", false);
    }

    /**
     * @brief Whether AFC unloads the toolhead automatically after a print.
     *
     * Unlike CFS/IFS (which always unload via firmware), AFC's post-print
     * behavior depends on the user's end-of-print macros. This is driven by
     * the per-printer SettingsManager toggle get_afc_unload_after_print()
     * (default false). When enabled, the pre-print runout warning is
     * suppressed because an empty toolhead is expected, not a fault.
     */
    [[nodiscard]] bool auto_unloads_after_print() const override;

    // Endless Spool support
    /**
     * @brief Get endless spool capabilities for AFC
     *
     * Available, always enabled (AFC has no on/off switch - a lane either names
     * a runout lane or it does not), and PerSlot editable via `SET_RUNOUT`.
     *
     * @note Holds no lock: every field is a constant for this backend.
     */
    [[nodiscard]] helix::printer::EndlessSpoolCapabilities
    get_endless_spool_capabilities() const override;

    /**
     * @brief Get the endless spool relation for all lanes
     *
     * AFC's `runout_lane` is a directed lane->lane edge held in the
     * SlotRegistry, so this is `endless_spool_config_from_edges()` over
     * `slots_.backup_edges()` - one ordered two-member group per configured
     * lane.
     *
     * @note Takes `mutex_`; callers must NOT hold it.
     */
    [[nodiscard]] helix::printer::EndlessSpoolConfig get_endless_spool_config() const override;

    /**
     * @brief Reset all tool mappings to defaults
     *
     * Uses AFC_RESET_MAPPING RUNOUT=no (RESET_AFC_MAPPING before the virtual-
     * tools firmware, Klipper-Add-On #832, which deregistered the old name) to
     * reset tool-to-lane mappings while preserving existing endless spool
     * configuration.
     *
     * @return AmsError with result
     */
    AmsError reset_tool_mappings() override;

    // Tool Mapping support
    /**
     * @brief Get tool mapping capabilities for AFC
     *
     * AFC supports per-lane tool assignment via SET_MAP G-code.
     *
     * @return Capabilities with supported=true, editable=true
     */
    [[nodiscard]] helix::printer::ToolMappingCapabilities
    get_tool_mapping_capabilities() const override;

    /**
     * @brief Get current tool-to-slot mapping
     *
     * Returns the tool_to_slot_map from system_info_.
     *
     * @return Vector where index=tool, value=slot
     */
    [[nodiscard]] std::vector<int> get_tool_mapping() const override;

    /// AFC echoes each lane's `map` field back over the subscription, so a
    /// restore can be confirmed against firmware truth rather than our own
    /// optimistic write (#1270).
    [[nodiscard]] bool reports_firmware_tool_mapping() const override {
        return true;
    }

    [[nodiscard]] uint64_t firmware_tool_mapping_generation() const override;

    /**
     * @brief Set discovered lane and hub names from PrinterCapabilities
     *
     * Called before start() to provide lane names discovered from printer.objects.list.
     * These are used as a fallback when the lane_data database is not available
     * (AFC versions < 1.0.32).
     *
     * For v1.0.32+, query_lane_data() may override/supplement this data.
     *
     * @param lane_names Lane names from PrinterCapabilities::get_afc_lane_names()
     * @param hub_names Hub names from PrinterCapabilities::get_afc_hub_names()
     */
    void set_discovered_lanes(const std::vector<std::string>& lane_names,
                              const std::vector<std::string>& hub_names) override;

    void set_discovered_sensors(const std::vector<std::string>& sensor_names) override;

    // Device-Specific Actions
    /**
     * @brief Get available device sections for AFC backend
     *
     * AFC exposes calibration and speed settings sections.
     *
     * @return Vector of DeviceSection for UI grouping
     */
    [[nodiscard]] std::vector<helix::printer::DeviceSection> get_device_sections() const override;

    /**
     * @brief Get available device actions for AFC backend
     *
     * Returns AFC-specific actions including:
     * - Calibration wizard
     * - Bowden length configuration
     * - Speed multipliers (forward/reverse)
     *
     * @return Vector of DeviceAction for UI rendering
     */
    [[nodiscard]] std::vector<helix::printer::DeviceAction> get_device_actions() const override;

    /**
     * @brief Execute an AFC-specific device action
     *
     * @param action_id Action identifier from get_device_actions()
     * @param value Optional value for sliders/toggles
     * @return AmsError indicating success or failure
     */
    AmsError execute_device_action(const std::string& action_id,
                                   const std::any& value = {}) override;

  protected:
    /**
     * @brief Transport for one endless-spool edge: `SET_RUNOUT LANE=x RUNOUT=y`.
     *
     * Ranges, self-backup and editability are already settled by
     * AmsBackend::set_endless_spool_backup(). This resolves the two lane names,
     * screens them for G-code injection, sends, and only then mirrors the edge
     * into the SlotRegistry - the old order updated the registry first, so a
     * rejected write left the registry claiming a backup the printer never got.
     *
     * @note Takes `mutex_` twice (name lookup, then the post-send mirror) and
     *       holds it across neither the injection check nor the G-code send.
     */
    AmsError apply_endless_spool_backup(int slot_index, int backup_slot) override;

    // Allow test helper access to private members
    friend class AmsBackendAfcTestHelper;
    friend class AfcPerSlotLoadedHelper;
    friend class AfcHelper;
    friend class AfcBypassPublishTestAccess;
    friend class AfcCurrentErrorHelper;
    friend class AfcRetainsHelper;
    friend class AfcLaneDataClearHelper;
    friend class AfcRebindHelper;
    friend class AfcFaultEventCharHelper;
    friend class AfcFeatureLevelHelper;
    friend class AfcFixtureHelper;
    friend class AmsBackendAfcEndlessSpoolHelper;
    friend class AmsBackendAfcMultiUnitHelper;
    friend class HubSensorTestHelper;
    friend class AmsBackendAfcMultiExtruderHelper;
    friend class AmsBackendAfcConfigHelper;
    friend class AfcErrorHandlingHelper;
    friend class AfcErrorStateHelper;
    friend class AfcCharHelper;
    friend class AfcToolchangeTestHelper;
    friend class AfcToolchangerLaneHelper;
    friend class AfcStateStringHelper;
    friend class AfcDatabaseResponseHelper;
    friend class AfcStatusFieldHelper;
    friend class AfcActionTimeoutHelper;
    friend class AfcDispatchAckHelper;
    friend class AfcToolchangerStatusHelper;
    friend class AfcStatusDispatchHelper;
    friend class AfcEjectPrintGateHelper;
    friend class AfcSharedExtruderHelper;
    friend class AfcLaneDataToolKeyHelper;
    friend class AfcReassertHelper;
    friend class AfcDelegatesHomingHelper;
    friend class AfcDispatchHelper;

    // --- AmsSubscriptionBackend hooks ---
    void on_started() override;
    void handle_status_update(const nlohmann::json& notification) override;
    const char* backend_log_tag() const override {
        return "[AMS AFC]";
    }

    /// dispatch_operation() sets the optimistic action (begin_dispatch_locked)
    /// BEFORE calling ensure_homed_then() -- on decline, the base class's
    /// generic IDLE reset alone leaves pending_dispatch_action_ armed and
    /// operation_detail stale, so route through abandon_dispatch() instead,
    /// the same unwind dispatch_operation()'s own `if (!result)` net uses.
    void on_home_confirmation_declined() override;

  private:
    // === User-attached slot identity (FilamentSlotOverrideStore) =============
    //
    // AFC firmware cannot hold brand, color_name or the Spoolman filament/vendor
    // ids — verified against a live lane payload and its lane_data record — so
    // those live only here. spool_name and total_weight_g are partial exceptions
    // from AFC v1.2.0, which publishes filament_name and initial_weight, but only
    // for a lane with a live Spoolman link; a user's override still outranks both.
    //
    // The namespace is PRIVATE, deliberately NOT the shared "lane_data": AFC's
    // own plugin owns that one, deletes the whole namespace on every Klipper
    // boot and full-POSTs each lane record, so our data would not survive and
    // AFC's records would be ingested as if the user had authored them.
    //
    // This is also what restores retention across an eject now that
    // parse_afc_stepper honours AFC's clears: firmware truth clears, and the
    // override re-supplies the identity the user attached.
    static constexpr const char* OVERRIDE_NAMESPACE = "helix-screen-afc-overrides";
    std::unique_ptr<helix::ams::FilamentSlotOverrideStore> override_store_;
    /// Store on the SHARED lane_data namespace, used only by
    /// publish_external_spool_lane. AFC's plugin owns that namespace — our
    /// private override_store_ is deliberately NOT pointed at it.
    std::unique_ptr<helix::ams::FilamentSlotOverrideStore> lane_publish_store_;
    std::unordered_map<int, helix::ams::FilamentSlotOverride> overrides_;
    /// Layer the user override over firmware values. Callers hold mutex_.
    void apply_overrides(SlotInfo& slot, int slot_index);
    /// Build + persist an override from a user edit. Callers hold mutex_.
    void persist_override(int slot_index, const SlotInfo& info);

    /// Async callback safety guard. Tokens shared with AfcConfigManager instances.
    helix::AsyncLifetimeGuard lifetime_;

    /**
     * @brief Parse AFC state from Moonraker JSON
     *
     * Extracts afc object from notification and updates system_info_.
     *
     * @param afc_data JSON object containing printer.afc data
     * @param deferred_error_event Output: error message to emit after releasing mutex
     * @param current_slot_set_by_afc_state Output: the AFC object spoke about the
     *        slot this frame, either direction. Suppresses the reconciliation block.
     * @param afc_stated_unloaded Output: the AFC object said specifically that
     *        NOTHING is at the toolhead (current_load/current_lane went null).
     *        Narrower than the flag above, which a named current_load also sets.
     *        Consumed only by apply_mount_state().
     */
    void parse_afc_state(const nlohmann::json& afc_data, std::string& deferred_error_event,
                         bool& current_slot_set_by_afc_state, bool& afc_stated_unloaded);

    /**
     * @brief Apply an AFC state string to action + operation_detail
     *
     * Maps the raw firmware string to an AmsAction via normalized matching and
     * sets a human, translated detail string (never the raw wire token — AFC
     * emits camelCase since v1.2.0 and operation_detail reaches the UI
     * verbatim). Warns once per distinct unrecognized string so a rewording
     * upstream surfaces in logs instead of silently reading as IDLE.
     *
     * @param raw    Raw state string from AFC
     * @param source Field it came from ("status" / "current_state"), for logs
     */
    void apply_state_string(const std::string& raw, const char* source);

    // === Stuck-action timeout ==============================================
    //
    // AFC's busy state is re-derived from the firmware-echoed state string on
    // every status frame. When the terminating frame never arrives — a macro
    // that silently never completes, a WebSocket bounce mid-toolchange, a
    // Klipper shutdown — the UI stays pinned busy forever with nothing to
    // release it (#1188). These budgets flip a genuinely stuck operation to
    // ERROR, which is what the AmsPanel ams_action observer and the sidebar
    // both key off.
    //
    // Every value is a UI backstop, not a progress tracker: AFC operations are
    // legitimately long (a measured BoxTurtle toolchange including cut, poop,
    // kick, brush and purge ran 67s) and a false timeout is far worse than a
    // late one, so all of them are generous.

    /// Everything busy that has no dedicated budget: RESETTING, FORMING_TIP,
    /// CUTTING, CHECKING. These are short, bounded macro steps.
    static constexpr int ACTION_TIMEOUT_SECONDS = 120;
    /// AFC's toolchanger states (ToolSwap, ToolDock, ToolPickup, Moving,
    /// Restoring) all map to SELECTING, and a full toolchange is the longest
    /// thing AFC does — dock, pick up, load, cut, purge, brush, restore
    /// position. AD5X's busy list omits SELECTING entirely (its firmware never
    /// reports it); AFC needs it and needs it long.
    static constexpr int SELECTING_TIMEOUT_SECONDS = 300;
    /// Cold nozzle to 300°C for high-temp materials. Klipper's own
    /// verify_heater aborts a genuinely dead heater well inside this.
    static constexpr int HEATING_TIMEOUT_SECONDS = 300;
    /// A multi-colour purge legitimately runs minutes on a large purge volume.
    static constexpr int PURGING_TIMEOUT_SECONDS = 240;
    /// Lane-to-toolhead feed over a long bowden, plus AFC's per-lane speed
    /// ramps. Shared by LOADING and UNLOADING — the paths are symmetric.
    static constexpr int LOAD_UNLOAD_TIMEOUT_SECONDS = 180;

    /// Start of the current action, stamped once per status frame in
    /// parse_afc_state() when the action changed across the whole frame.
    std::chrono::steady_clock::time_point action_start_time_{std::chrono::steady_clock::now()};

    /// Raw AFC state string most recently applied by apply_state_string().
    /// Keys the timeout latch; NOT a display value.
    std::string last_raw_state_;

    /// Raw AFC state string that blew its budget, and the detail composed when
    /// it did. While set, the normal state->action mapping is overridden with
    /// ERROR.
    ///
    /// The latch exists because AFC — unlike AD5X, which drives its own action
    /// state machine — re-derives the action from the firmware string every
    /// frame. Setting ERROR alone would be undone by the very next frame, whose
    /// unchanged "Loading" maps straight back to LOADING and restarts the
    /// clock, flapping between busy and ERROR indefinitely. Released the moment
    /// AFC reports a genuinely different string, and by clear_fault().
    std::optional<std::string> timed_out_state_;
    std::string timed_out_detail_;

    /// Flip a busy action that has outlived its budget to ERROR and latch it.
    /// No-op while already latched, and for IDLE / ERROR / PAUSED. Caller holds
    /// mutex_.
    void check_action_timeout();

    /// Re-assert (or release) the timeout latch after the frame's state strings
    /// have been applied. Caller holds mutex_.
    void apply_action_timeout_latch_locked();

    // === Optimistic dispatch + macro-ack resolution ========================
    //
    // AFC answers a command it has nothing to do about — "lane3 already
    // loaded" — by acking in 4ms without ever entering a toolchange, so
    // current_state never leaves "Idle" (#1183). The UI's completion path keys
    // entirely on an ams_action transition and AmsState::sync_from_backend()
    // short-circuits on an unchanged value, so a no-op produced no notify at
    // all and nothing could end the operation.
    //
    // Two halves, both of which other backends already have: set the action
    // optimistically at dispatch so there is a transition to complete, and
    // resolve it on the macro's own gcode ack (the same signal AD5X IFS uses in
    // finalize_op_after_macro()).

    /// Action the most recent dispatch set optimistically, cleared once that
    /// dispatch has been resolved — by its ack, by AFC taking the operation
    /// over, or by a newer dispatch superseding it.
    std::optional<AmsAction> pending_dispatch_action_;

    /// Monotonic dispatch counter. The ack carries the generation it was issued
    /// under, so an ack that lands after a newer dispatch resolves nothing.
    uint64_t dispatch_generation_{0};

    /// Set @p action optimistically for a dispatch that is about to go out and
    /// return the generation its ack must present. Caller holds mutex_.
    uint64_t begin_dispatch_locked(AmsAction action);

    /// Undo an optimistic set whose gcode never left the building. Caller must
    /// NOT hold mutex_.
    void abandon_dispatch(uint64_t generation);

    /// Resolve a dispatch to IDLE on its macro's gcode ack, but only when AFC
    /// never took the operation over. Main thread; must NOT hold mutex_.
    void finalize_dispatch_after_macro(uint64_t generation);

    /// Send a filament operation: set @p action optimistically, dispatch
    /// @p gcode through ensure_homed_then(), and resolve on the macro's ack.
    /// Caller must NOT hold mutex_ and must have passed check_preconditions().
    AmsError dispatch_operation(std::string gcode, AmsAction action);

    /**
     * @brief Query current AFC state from Moonraker
     *
     * Queries the current state of all AFC objects via printer.objects.query.
     * With the early hardware discovery callback architecture, this is typically
     * NOT needed - the backend receives initial state naturally from the
     * printer.objects.subscribe response.
     *
     * Available for manual re-query scenarios (e.g., recovery from errors).
     */
    void query_initial_state();

    /**
     * @brief Query lane data from Moonraker database
     *
     * AFC stores lane configuration in Moonraker's database under the
     * "AFC" namespace with key "lane_data".
     */
    void query_lane_data();

    /**
     * @brief Whether this machine has an AFC toolchanger.
     *
     * The registration gate for AFC_SELECT_TOOL / AFC_UNSELECT_TOOL: both are
     * registered by AfcToolchanger.__init__ (AFC_Toolchanger.py:47-53) and by
     * nothing else, and Klipper loads that class only for an
     * `[AFC_Toolchanger <name>]` section.
     *
     * Two INDEPENDENT sufficient signals, OR-ed — each one on its own implies
     * the section exists, and neither is necessary:
     *
     *  1. configfile.settings carries an `afc_toolchanger <name>` section. This
     *     is literally the registration condition, so it is authoritative.
     *  2. AFC's `units` array carries a unit whose type is `Toolchanger`. AFC
     *     publishes units as `"<Type> <name>"` with spaces in the type replaced
     *     by underscores (AFC.py get_status, v1.2.0:2550-2556), and the type is
     *     the string AfcToolchanger set on itself
     *     (`config.get("type", "Toolchanger")`).
     *
     * Signal 2 alone is NOT enough: that same loop only emits a unit when
     * `len(unit.lanes) > 0` (AFC.py v1.2.0:2554). A toolchanger whose toolheads
     * are all lane-fed (a BoxTurtle feeding several heads on a shuttle) has its
     * per-toolhead synthetic lane popped back off the unit by
     * AFCExtruder.check_lanes() (AFC_extruder.py:391-401) once a real lane
     * exists, so the Toolchanger unit can legitimately end up with zero lanes
     * and never appear in `units` at all. Signal 1 is what covers that machine.
     * Conversely signal 2 covers the case where configfile is unreachable.
     *
     * Distinct from `num_extruders_ > 1`, which counts `[AFC_extruder]`
     * sections and is therefore also true on IDEX and standalone-toolhead
     * machines that have no toolchanger and no AFC_SELECT_TOOL.
     *
     * Takes mutex_; callers must NOT hold it.
     */
    [[nodiscard]] bool has_toolchanger() const;

    /**
     * @brief Read the AFC topology that only the printer's config file carries.
     *
     * One-shot `printer.objects.query {"configfile": ["settings"]}`, recovering
     * two things no get_status() publishes:
     *
     *  - the AFC_extruder section -> Klipper extruder name map. AFC derives a
     *    toolhead's tool index from `th_extruder_name` (AFC_extruder.py:222-223
     *    = `config.get("extruder_name", <section name>)`, consumed at
     *    AFC_Toolchanger.py:231-232, both v1.2.0).
     *  - whether an `[AFC_Toolchanger <name>]` section exists at all, which is
     *    exactly what registers AFC_SELECT_TOOL.
     *
     * Costs one query of the whole resolved config at startup; the same query
     * the bed-mesh and screws-tilt panels already make on demand.
     */
    void query_afc_configfile_topology();

    /**
     * @brief Klipper extruder object name for an AFC_extruder section name.
     *
     * The two coincide on the common `[AFC_extruder extruder1]` shape and
     * diverge on anything else — `[AFC_extruder e0]\nextruder_name: extruder`.
     * AFC keys its own tool indices on the latter (AFC_extruder.py:223,
     * consumed at AFC_Toolchanger.py:231-232), so this is the only correct
     * bridge from what AFC's status frames name to what Klipper names.
     *
     * Returns @p section_name unchanged when configfile has not answered, which
     * is right for every standard install and the best available guess for the
     * rest. This is the single point where that substitution happens; nothing
     * outside it should be parsing an AFC-sourced extruder string.
     *
     * Caller must hold mutex_.
     */
    [[nodiscard]] std::string klipper_extruder_name_unlocked(const std::string& section_name) const;

    /**
     * @brief AFC_extruder SECTION name for a tool index, or "" if unknown.
     *
     * The inverse of tool_index_for_extruder_unlocked(). AFC registers its
     * per-extruder mux commands (UPDATE_TOOLHEAD_SENSORS, SAVE_EXTRUDER_VALUES,
     * AFC_SET_EXTRUDER_LED) on the SECTION name — `register_mux_command(...,
     * "EXTRUDER", self.name, ...)` where `self.name` is the section suffix
     * (AFC_extruder.py:221, :364-369) — so any G-code we address to an extruder
     * needs this direction, never the Klipper name.
     *
     * Caller must hold mutex_.
     */
    [[nodiscard]] std::string afc_extruder_section_for_tool_unlocked(int tool_index) const;

    /**
     * @brief Tool index for an AFC_extruder section name, or -1 if unknown.
     *
     * Resolves through klipper_extruder_name_unlocked() and then the single
     * `extruder<N>` grammar in helix::tool_number_for_extruder(). Returns -1
     * rather than guessing 0 when neither resolves — every toolhead silently
     * claiming T0 is worse than a toolhead with no attribution.
     *
     * Caller must hold mutex_.
     */
    [[nodiscard]] int tool_index_for_extruder_unlocked(const std::string& ext_name) const;

    /**
     * @brief Parse lane data from database response
     *
     * Processes the lane_data JSON object and updates system_info_.gates.
     *
     * @param lane_data JSON object containing lane configurations
     */
    void parse_lane_data(const nlohmann::json& lane_data);

    /**
     * @brief Detect AFC version by querying afc-install database namespace
     *
     * Queries Moonraker's database for the afc-install namespace which
     * contains version information. Sets afc_version_ and capability flags.
     */
    void detect_afc_version();

    /**
     * @brief Extract the payload of a server.database.get_item reply
     *
     * send_jsonrpc delivers the full JSON-RPC envelope, so the payload lives at
     * result.value. Strict about that shape: a payload is an arbitrary object,
     * so it cannot be told apart from an envelope in general. Replies obtained
     * via IMoonrakerAPI::database_get_item are already unwrapped and must not be
     * passed here.
     *
     * @return the payload, or a null json when absent
     */
    static const nlohmann::json& database_item_value(const nlohmann::json& response);

    /**
     * @brief Apply an afc-install database reply (sets version + capability flags)
     *
     * Split out from the RPC callback so the parse is testable without a live
     * client. Does not raise the version-warning modal or trigger the lane_data
     * query — the caller does both, keyed off the members this sets.
     *
     * @return true when a version string was found and applied
     */
    bool apply_afc_version_response(const nlohmann::json& response);

    /// Issue the one-shot unscoped query that check_afc_feature_level() needs.
    /// @param lane_object Full Klipper object name, e.g. "AFC_stepper lane1".
    void probe_feature_level(const std::string& lane_object);

    /// One-shot feature probe + upgrade advisory. Must be handed a COMPLETE lane
    /// object (see probe_feature_level), never a status frame.
    void check_afc_feature_level(const nlohmann::json& lane_status);

    /**
     * @brief Apply an AFC/lane_data database reply
     *
     * @return true when a lane_data object was found and parsed
     */
    bool apply_lane_data_response(const nlohmann::json& response);

    /**
     * @brief Parse AFC_stepper lane object for sensor states and filament info
     *
     * Caller passes the slot index directly (the enclosing loop already knows
     * it), so this function does not re-resolve the lane name through the
     * SlotRegistry hashtable. Avoids a hashtable find on every status update.
     *
     * @param slot_index Registry slot index for this lane (from enclosing loop)
     * @param lane_name Lane identifier (e.g., "lane1"), used for logging and
     *                  lane_hub_routing_ keying
     * @param data JSON object from AFC_stepper lane{N}
     */
    void parse_afc_stepper(int slot_index, const std::string& lane_name,
                           const nlohmann::json& data);

    /**
     * @brief Push a retained Spoolman binding back into AFC (#1289)
     *
     * Called from parse_afc_stepper on the empty -> loaded EDGE only. With
     * "Keep Spool Info on Eject" on, AFC's own remember_spool = false means
     * firmware dropped the lane's link on eject while our override kept the
     * identity — re-inserting the spool would paint the retained id here
     * while AFC/Mainsail show an unknown spool. Sends the same
     * SET_SPOOL_ID write the editor re-link uses, wrapped in
     * record_own_spool_write() so the echo cannot trip the merge's re-bind
     * clear.
     *
     * Gates: retention setting on, override holds a spool id for the lane,
     * and firmware's freshest spool_id reading is 0/null (a
     * remember_spool = true firmware or another writer's link wins by not
     * firing). Edge-triggered — no re-send while loaded, no retry on a
     * failed dispatch; the next eject/re-insert cycle is the retry.
     *
     * @param slot_index Registry slot index for this lane
     * @param lane_name Lane identifier (e.g., "lane1") for the gcode
     */
    void maybe_reassert_retained_spool_link(int slot_index, const std::string& lane_name);

    /**
     * @brief Parse AFC_hub object for per-hub sensor state
     *
     * @param hub_name Name of the hub (e.g., "Turtle_1")
     * @param data JSON object from AFC_hub
     */
    void parse_afc_hub(const std::string& hub_name, const nlohmann::json& data);

    /**
     * @brief Parse AFC_buffer object for buffer health and fault data
     *
     * Extracts fault_detection_enabled, distance_to_fault, state, the multiplier trio
     * and the lane list from the buffer status object, accumulating them into
     * buffer_health_[buffer_name] — Moonraker sends deltas, so absent fields must
     * leave the previous reading alone. Then re-derives the unit-level view via
     * apply_buffer_health_to_units(); a buffer sits between hub and toolhead, so its
     * health belongs to the unit, not to a slot.
     *
     * @param buffer_name Name of the buffer (e.g., "Turtle_1")
     * @param data JSON object from AFC_buffer
     */
    void parse_afc_buffer(const std::string& buffer_name, const nlohmann::json& data);

    /**
     * @brief Re-attach every known buffer's health to the unit that owns its lanes.
     *
     * Derives AmsUnit::buffer_health from buffer_health_ + buffer_lane_names_ rather
     * than mutating a unit in place, so the reading survives reorganize_slots()
     * rebuilding the unit vector and lands on the right unit once the multi-unit
     * layout exists. Buffers whose lanes resolve to no unit yet are simply skipped —
     * the next call picks them up.
     *
     * @pre mutex_ must be held by caller.
     */
    void apply_buffer_health_to_units();

    /**
     * @brief Parse AFC_extruder object for toolhead sensor states
     *
     * @param ext_name Klipper extruder name (e.g. "extruder", "extruder1")
     * @param data JSON object from AFC_extruder
     */
    void parse_afc_extruder(const std::string& ext_name, const nlohmann::json& data);

    /**
     * @brief Parse a unit-level Klipper object (AFC_BoxTurtle, AFC_OpenAMS)
     *
     * Reads lanes[], extruders[], hubs[], buffers[] arrays from the unit object
     * and derives topology (PARALLEL vs HUB) based on hub/extruder counts.
     *
     * @param unit_info The AfcUnitInfo to populate
     * @param data JSON object from the unit-level Klipper object
     */
    void parse_afc_unit_object(AfcUnitInfo& unit_info, const nlohmann::json& data);

    /**
     * @brief Rebuild unit_lane_map_ from unit_infos_ and reorganize slots
     *
     * Called after all unit-level objects have been parsed. Rebuilds the
     * unit-to-lane mapping from unit_infos_ and triggers reorganize_slots().
     */
    void rebuild_unit_map_from_klipper();

    /**
     * @brief Initialize slot structures based on discovered lanes
     *
     * Called when we first receive lane data to create the correct
     * number of SlotInfo entries.
     *
     * @param lane_names Vector of lane name strings (from AFC discovery)
     */
    void initialize_slots(const std::vector<std::string>& lane_names);

    /**
     * @brief Derive current_slot / filament_loaded from carriage mount state.
     *
     * The single authority for machines that have a carriage, run once at the
     * end of a status frame so it settles whatever the individual parsers
     * negotiated among themselves. No-op when mount_state is UNKNOWN, which is
     * every backend without a toolchanger.
     *
     * Exists because the per-parser writes are each guarded by
     * `current_slot < 0`, which made the FIRST writer permanent: on a
     * toolchanger a parked toolhead's lane was elected and then never released,
     * so the UI named a slot no source claimed (#1229). Deliberately
     * unconditional — a guard here would reintroduce the latch.
     *
     * Caller must hold mutex_.
     */
    /// @param afc_stated_unloaded The AFC object said nothing is at the toolhead
    ///        THIS frame. The carriage still decides which tool is current; it
    ///        does not get to elect a slot AFC just said holds nothing.
    void apply_mount_state(bool extruder_set_active_slot, bool afc_stated_unloaded);

    /**
     * @brief Reorganize slots into multi-unit structure using unit_lane_map_
     *
     * When AFC reports multiple units with per-unit lane assignments,
     * this method rebuilds system_info_.units to reflect the actual
     * multi-unit hardware topology. Preserves existing slot data
     * (colors, materials, etc.) during reorganization.
     *
     * Called from parse_afc_state() when unit_lane_map_ is populated
     * and slots are already initialized.
     */
    void reorganize_slots();

    /**
     * @brief Compute filament segment from sensor states (no locking)
     *
     * Internal helper called from locked contexts to avoid deadlock.
     * Uses slots_ sensors, hub_sensors_, tool_start_sensor_, tool_end_sensor_.
     *
     * @return PathSegment indicating filament position
     */
    [[nodiscard]] PathSegment compute_filament_segment_unlocked() const;

    /// Whether the extruder is free of filament. Caller holds mutex_.
    /// Combines the physical toolhead sensors, AFC.current (current_load) and
    /// per-lane tool_loaded rather than system_info_.filament_loaded, which is
    /// derived from current_lane and so reads true for the whole of a toolchange.
    [[nodiscard]] bool toolhead_is_free_unlocked() const;

    /// Whether active_load_lane_ names a lane that still exists in slots_.
    /// Caller holds mutex_. Shared by can_recover_lane_position() and
    /// lane_recovery_is_attributed() so the recovery gate and the UI-facing
    /// attribution flag cannot drift apart on a stale lane name.
    [[nodiscard]] bool recovery_attribution_valid_unlocked() const;

    /// Build the applicable recovery actions for an AFC pause/jam, reading live
    /// toolhead state. Caller holds mutex_ (the base declares that contract;
    /// mutex_ is non-recursive, so this must not lock). Offers Unload only when
    /// the toolhead is loaded; Eject only when empty and a lane is selected.
    [[nodiscard]] std::vector<helix::RecoveryAction> build_recovery_actions() const override;

    /**
     * @brief Execute a G-code command with user-facing toast notifications
     *
     * Like execute_gcode() but shows a success or error toast when the
     * async Moonraker callback fires. Thread-safe (uses ui_queue_update()).
     *
     * @param gcode The G-code command to execute
     * @param success_msg Toast message on success (empty = no toast)
     * @param error_prefix Toast prefix on error (shown as "prefix: error details")
     * @return AmsError indicating success or failure to queue command
     */
    virtual AmsError execute_gcode_notify(const std::string& gcode, const std::string& success_msg,
                                          const std::string& error_prefix);

    /**
     * @brief Validate slot index is within range
     *
     * @param slot_index Slot index to validate
     * @return AmsError (SUCCESS if valid, INVALID_SLOT otherwise)
     */
    AmsError validate_slot_index(int slot_index) const;

    // Unified slot registry -- single source of truth for all slot-indexed state
    helix::printer::SlotRegistry slots_;

    // Pre-init storage for lane names from PrinterCapabilities discovery.
    // Consumed by initialize_slots() then cleared; after init use slots_.name_of().
    std::vector<std::string> discovered_lane_names_;

    // Unit-to-lane mapping (populated from AFC unit data)
    // Key: unit name, Value: lane names belonging to that unit
    std::unordered_map<std::string, std::vector<std::string>> unit_lane_map_;

    // Reported AFC version. DISPLAY AND DIAGNOSTICS ONLY — never gate behavior on
    // it. AFC removed the code that writes the afc-install namespace (its commit
    // 7d20db7, #451, 2025-06-16), so this is either "unknown" or a value frozen
    // before that date. Detect capabilities from the data instead.
    std::string afc_version_{"unknown"};

    /// Latch for the feature probe: it costs a query, so it runs exactly once.
    bool feature_level_checked_{false};

    /// "AFC_stepper " or "AFC_lane ", learned from the first status frame that
    /// carries a lane. Only the prefix is learnable from a frame; the fields are
    /// not, because the subscription is field-scoped.
    std::string lane_object_prefix_;

    // Per-lane hub routing: lane_name → hub name ("direct" for direct lanes)
    std::unordered_map<std::string, std::string> lane_hub_routing_;

    // Lanes whose "map" field listed more than one tool (AFC virtual tools, #605)
    // — dedupes the "extras are not tracked" warning so it fires once per lane,
    // not once per status update.
    std::set<std::string> multi_tool_warned_lanes_;

    // lane_name → tool number AFC last reported in "current_map" (virtual tools,
    // #605). Remembered across updates because Moonraker sends deltas and the two
    // fields move independently: AFC_ADD_MAPPING sends "map" alone, a tool change
    // within a lane sends "current_map" alone. Without this, a map-only delta would
    // fall back to the lowest tool and yank the lane off the one AFC is driving.
    // Cleared when the lane is unmapped, and ignored whenever the tool is no longer
    // a member of a present "map".
    std::unordered_map<std::string, int> lane_current_tool_;

    // A lane_data payload keyed by T(n) (virtual-tools firmware, #832) that
    // arrived while no tool mapping could resolve its records. Kept whole and
    // replayed once parse_afc_stepper() lands a mapping, because
    // query_lane_data() is one-shot and never retried. Cleared (re-parked) by
    // the replay itself if records still resolve to nothing.
    std::optional<nlohmann::json> pending_tool_lane_data_;

    // Slots whose tool mapping the FIRMWARE asserted via a lane "map" field.
    // initialize_slots() seeds a 1:1 identity placeholder, and a T(n)-keyed
    // lane_data join that trusted it could paint another lane's spool onto a
    // slot nothing corrects until a real map arrives. parse_lane_data() only
    // resolves tool keys through slots in this set; everything else parks.
    // Reset with the registry in initialize_slots().
    std::set<int> firmware_mapped_slots_;

    // The virtual-tools firmware renamed RESET_AFC_MAPPING → AFC_RESET_MAPPING
    // (#832) and deregistered the old name. Detected by key PRESENCE of
    // multiple_tool_mapping in the AFC status object — the value is the user's
    // opt-in to virtual tools and defaults false, so only presence is a version
    // signal.
    bool afc_reset_mapping_renamed_{false};

    // AFC state strings outside our known vocabulary — dedupes the schema-drift
    // warning so it fires once per distinct string, not once per status update.
    std::set<std::string> unknown_state_warned_;

    // Per-tool toolchanger state, keyed by AFC_extruder name (AFC v1.2.0 #768).
    std::unordered_map<std::string, AfcToolState> tool_states_;

    // Hub and toolhead sensors (from AFC_hub and AFC_extruder objects)
    std::unordered_map<std::string, bool> hub_sensors_; ///< Per-hub sensor state, keyed by hub name
    bool tool_start_sensor_{false};                     ///< Toolhead entry sensor
    bool tool_end_sensor_{false};                       ///< Toolhead exit/nozzle sensor

    /// Per-extruder toolhead sensors + the lane each names as loaded, keyed by
    /// AFC_extruder name. tool_start_sensor_ / tool_end_sensor_ above stay as
    /// the whole-system view every other caller wants; this map is what lets
    /// slot_has_filament_at_toolhead() answer per lane.
    std::unordered_map<std::string, AfcExtruderSensors> extruder_sensors_;

    /// Lane AFC currently names as active, verbatim from AFC.current_load or
    /// AFC.current_lane; empty when AFC names neither. Distinct from
    /// system_info_.current_slot, which is derived from several sources and may
    /// be stale. Used only to attribute the shared hub sensor to a lane.
    std::string active_load_lane_;

    /// Lane AFC reports as gripped by the extruder, verbatim from
    /// AFC.current_load (= AFC.current, maintained by set_loaded() /
    /// set_unloaded()); empty when none. Narrower than active_load_lane_ above,
    /// which prefers AFC.current_lane (= AFC.current_loading) and so names the
    /// lane a toolchange is WORKING ON whether or not it reached the toolhead.
    /// Used by toolhead_is_free_unlocked() to mirror upstream's own guard.
    std::string toolhead_lane_;

    /// Remaining AFC_CLEAR_MESSAGE sends allowed for the in-flight clear_fault().
    /// printer.AFC.message is a FIFO head and each clear pops one entry, so a
    /// single send leaves the next queued error on screen. Armed by clear_fault(),
    /// spent one per status delta that still carries a message. Hitting zero is
    /// the abnormal exit — see MESSAGE_DRAIN_MAX_CLEARS.
    int message_drain_budget_ = 0;

    /// Set by parse_afc_state() while holding mutex_; consumed by
    /// handle_status_update() after the lock is released. parse_afc_state() must
    /// never send gcode itself — same reason deferred_error_event exists.
    bool message_drain_pending_ = false;

    /// When the current drain arm expires. printer.AFC.message is a delta field:
    /// if the queue was already empty at clear_fault() time, no later delta will
    /// carry `message` at all, so the empty-message disarm never fires and the
    /// budget would otherwise persist indefinitely — silently popping the user's
    /// next unrelated error. A wall-clock bound is what "window" actually means.
    std::chrono::steady_clock::time_point message_drain_deadline_{};

    /// Runaway guard, NOT the expected stopping point. The drain terminates on
    /// an empty `AFC.message.message` (parse_afc_state()) or on the wall-clock
    /// deadline above; this cap only stops a fault that re-enqueues as fast as
    /// we pop from spinning. Overshoot is safe — clearing an empty queue is a
    /// no-op.
    ///
    /// Sized off observed queue depth, not off one fault. Every
    /// AFC_logger.error() and .warning() call appends an entry (one per call —
    /// the per-line loop in AFC_logger.py writes the log file, not the queue,
    /// verified against the add-on source 2026-07-29), and nothing pops them
    /// implicitly: reset_failure() and AFC_RESUME both leave message_queue
    /// untouched. Entries therefore accumulate across a whole session — one
    /// real failure reached depth 4 with a per-print-start SET_AFC_TOOLCHANGES
    /// deprecation warning sitting at the head, hiding the actionable error
    /// behind it. A cap of two stopped mid-queue and left the residue to
    /// surface as the next session's stale error (#1186).
    ///
    /// The cost of a larger cap is bounded by the deadline, not by this number.
    /// The re-arm check cannot distinguish a backlogged message from one
    /// generated *after* the clear — AFC exposes only the queue head via
    /// _get_message(clear=False) — so an error raised by the caller's own
    /// follow-up action (the sidebar sends AFC_RESET right after clear_fault())
    /// can be swallowed unseen anywhere inside the 5 s window.
    static constexpr int MESSAGE_DRAIN_MAX_CLEARS = 10;

    /// Sends one queued AFC_CLEAR_MESSAGE if the drain is armed and a message is
    /// still present. Must be called WITHOUT mutex_ held.
    void maybe_drain_message_queue();

    // Global state
    bool error_state_{false};       ///< AFC error state
    bool bypass_active_{false};     ///< Bypass mode active (external spool)
    bool afc_quiet_mode_{false};    ///< AFC quiet mode state
    bool afc_led_state_{false};     ///< AFC LED state
    std::string current_lane_name_; ///< Currently active lane name
    // Two dedup trackers: last_error_msg_ prevents duplicate emit_event(EVENT_ERROR),
    // last_seen_message_ prevents duplicate toast/notification display. Both reset
    // when the AFC message field clears.
    std::string last_error_msg_;
    std::string last_seen_message_;
    std::string last_message_type_;      ///< Type of last system message ("error", "warning", etc.)
    std::vector<std::string> hub_names_; ///< Discovered hub names
    std::vector<std::string> buffer_names_; ///< Discovered buffer names
    float bowden_length_{450.0f};           ///< Bowden tube length from hub (default 450mm)

    /// T-commands AFC has actually registered with Klipper (AFC.maps, v1.2.0+).
    /// Kept as a cross-check against the mapping we derive from each lane's
    /// `map` field: a lane claiming T3 that AFC never registered means the
    /// gcode we would send does not exist.
    std::vector<std::string> afc_tool_cmds_;

    /// Tool numbers already reported as unregistered — dedupes the cross-check
    /// warning so it fires once per tool, not once per status frame.
    std::set<int> tool_cmd_missing_warned_;

    /// Per-lane `remember_spool`: when true AFC keeps the lane's spool metadata
    /// across an eject instead of running clear_values(). Tells us whether the
    /// firmware or our own override store is the thing preserving identity.
    std::unordered_map<std::string, bool> lane_remember_spool_;

    /// Firmware's own last-reported spool_id per lane, updated only when a
    /// status delta carries the key. Distinct from SlotInfo::spoolman_id,
    /// which the §5 override merge re-supplies with the retained id — this
    /// map preserves "does AFC itself still hold a link?" for
    /// maybe_reassert_retained_spool_link() (#1289).
    std::unordered_map<std::string, int> lane_firmware_spool_id_;

    /// Lanes last seen on each buffer, keyed by buffer name. AFC's buffer status
    /// arrives as a Moonraker delta, so a frame that changes only `state` omits
    /// `lanes` — without this cache the health update could not be routed to a
    /// unit and would be dropped.
    std::unordered_map<std::string, std::vector<std::string>> buffer_lane_names_;

    /// What AFC last reported for each buffer, keyed by buffer name. This is the
    /// record; AmsUnit::buffer_health is a derived view of it. Accumulating into
    /// the unit instead was wrong twice over: reorganize_slots() rebuilds every
    /// AmsUnit from scratch, so the reading survived only until the next frame
    /// carrying a unit object, and on a multi-unit rig whose layout was not built
    /// yet every buffer resolved to unit 0 and read-modify-wrote the previous
    /// buffer's fields.
    std::unordered_map<std::string, BufferHealth> buffer_health_;

    /// Unit index each buffer was last attached to (-1 = unresolved), so the
    /// attribution can be logged when it CHANGES rather than on every
    /// re-derivation. apply_buffer_health_to_units() runs on every status frame
    /// that carries a buffer or a unit object; logging unconditionally there put
    /// 75 extra lines into a five-unit rig's log for one three-frame replay.
    std::unordered_map<std::string, int> buffer_unit_attribution_;

    // Multi-extruder (toolchanger) state
    int num_extruders_{1}; ///< Number of extruders (1 = standard, 2+ = toolchanger)
    std::vector<AfcExtruderInfo>
        extruders_; ///< Per-extruder info (populated from system.extruders)

    // Unit-level info from flat string units and unit Klipper objects
    std::vector<AfcUnitInfo> unit_infos_; ///< Parsed from flat string "Type Name" units

    /// AFC_extruder SECTION names from the top-level AFC.extruders array, e.g.
    /// {"extruder", "extruder1", …} on a stock config and {"e0", "e1", …} on a
    /// renamed one. These are Klipper object keys ("AFC_extruder " + name) and
    /// G-code mux keys, NOT Klipper extruder names — go through
    /// klipper_extruder_name_unlocked() before deriving a tool number.
    std::vector<std::string> extruder_names_;

    /// AFC_extruder SECTION name -> Klipper extruder name, read from
    /// configfile.settings["afc_extruder <section>"]["extruder_name"]. Empty
    /// until query_afc_configfile_topology() answers, and stays empty on a
    /// printer whose config Moonraker will not serve.
    std::unordered_map<std::string, std::string> extruder_klipper_names_;

    /// True once configfile.settings has actually been read, whatever it held.
    /// Distinguishes "this section has no extruder_name" from "we have not
    /// looked yet" — an empty extruder_klipper_names_ means both, and only the
    /// first justifies telling the user their config is missing something.
    bool configfile_answered_{false};

    /// True once an `[AFC_Toolchanger …]` section has been seen in
    /// configfile.settings. Never cleared by a config that lacks one — absence
    /// in an unanswered/unavailable configfile is not evidence, and the units
    /// array is the independent second signal. See has_toolchanger().
    bool configfile_has_toolchanger_{false};

    /// Section names already reported as unresolvable — dedupes the warning so
    /// it fires once per extruder, not once per status frame.
    mutable std::set<std::string> extruder_tool_index_warned_;

    // Per-extruder toolhead LED state (for multi-extruder mode)
    std::unordered_map<int, bool> toolhead_led_state_; ///< tool_index → LED on/off

    // Path visualization state
    PathSegment error_segment_{PathSegment::NONE}; ///< Inferred error location

    // helix::Config file managers (lazy-loaded on first device action access)
    std::unique_ptr<AfcConfigManager> afc_config_;        ///< AFC/AFC.cfg
    std::unique_ptr<AfcConfigManager> macro_vars_config_; ///< AFC/AFC_Macro_Vars.cfg
    std::atomic<bool> configs_loading_{false};            ///< Currently loading config files
    std::atomic<bool> configs_loaded_{
        false}; ///< helix::Config files have been loaded (acquire/release barrier)

    /// Load AFC config files from printer
    void load_afc_configs();

    /// Detect tip method (cut vs tip-form) from loaded AFC config.
    /// Temporary: will be replaced by direct Moonraker status query when AFC
    /// exposes tool_cut/form_tip in get_status().
    void update_tip_method_from_config();

    /// Helper to get macro variable as float
    float get_macro_var_float(const std::string& key, float default_val) const;
    /// Helper to get macro variable as bool
    bool get_macro_var_bool(const std::string& key, bool default_val) const;

    // ------------------------------------------------------------------
    // LANE_UNLOAD serialization
    //
    // Firing several LANE_UNLOAD macros back-to-back overlaps their lane LED
    // animations and stepper steps on AFC's MCU (Turtle_1), which contributed
    // to Klippy "Timer too close" shutdowns when a user tapped Eject on
    // multiple lanes in quick succession. We instead queue eject requests and
    // run them one at a time — when the in-flight LANE_UNLOAD completes
    // (Moonraker fires our success/error callback), we pop the next from the
    // queue and dispatch it.
    // ------------------------------------------------------------------
    std::mutex eject_queue_mutex_;
    std::deque<std::string> pending_eject_lanes_;
    bool eject_in_flight_{false};

    /// Queue or immediately fire a LANE_UNLOAD for the given lane. Returns
    /// success on enqueue — the caller does not need to re-issue if a previous
    /// eject is still running.
    AmsError enqueue_lane_unload(const std::string& lane_name);

  protected:
    /// Dispatch a LANE_UNLOAD via api_->execute_gcode with completion callbacks
    /// that drain the queue. Caller must have set eject_in_flight_ = true.
    /// Virtual + protected so tests can override (api_ is null in unit tests).
    virtual void dispatch_lane_unload(const std::string& lane_name);

    /// Called from the gcode-completion callback (success or error). Pops the
    /// next queued eject (if any) and dispatches it; otherwise clears the
    /// in-flight flag. Protected so test overrides of dispatch_lane_unload can
    /// signal completion.
    void on_lane_unload_done();
};
