// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_subscription_backend.h"
#include "filament_slot_override_store.h"
#include "toolchanger_addon.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class ToolChangerTestAccess;

/**
 * @file ams_backend_toolchanger.h
 * @brief Physical tool changer backend implementation
 *
 * Implements the AmsBackend interface for physical tool changers using
 * viesturz/klipper-toolchanger. Unlike filament-switching systems (Happy Hare,
 * AFC), tool changers have multiple physical toolheads that are swapped.
 *
 * Key differences from filament systems:
 * - Each "slot" is a complete toolhead with its own extruder
 * - No hub/selector - path topology is PARALLEL
 * - "Loading" means mounting the tool to the carriage
 * - No bypass mode (each tool IS the path)
 *
 * Klipper Objects (viesturz/klipper-toolchanger):
 * - toolchanger.status     (string): "ready", "changing", "error", "uninitialized"
 * - toolchanger.tool       (string): Current tool name ("T0") or null
 * - toolchanger.tool_number (int): Current tool number (-1 if none)
 * - toolchanger.tool_numbers (array[int]): All tool numbers [0, 1, 2]
 * - toolchanger.tool_names  (array[string]): All tool names ["T0", "T1", "T2"]
 *
 * Per-tool Objects:
 * - tool T0.active         (bool): Is this tool selected?
 * - tool T0.mounted        (bool): Is this tool mounted on carriage?
 * - tool T0.gcode_x_offset (float): X offset for tool
 * - tool T0.gcode_y_offset (float): Y offset for tool
 * - tool T0.gcode_z_offset (float): Z offset for tool
 * - tool T0.extruder       (string): Associated extruder name
 * - tool T0.fan            (string): Associated fan name
 *
 * G-code Commands:
 * - SELECT_TOOL TOOL=T{n}  - Mount specified tool
 * - UNSELECT_TOOL          - Unmount current tool (park it)
 * - T{n}                   - Tool change macro (same as SELECT_TOOL)
 */
class AmsBackendToolChanger : public AmsSubscriptionBackend {
  public:
    /**
     * @brief Construct tool changer backend
     *
     * @param api Pointer to IMoonrakerAPI (for sending G-code commands)
     * @param client Pointer to helix::IMoonrakerClient (for subscribing to updates)
     *
     * @note Pointers must remain valid for the lifetime of this backend.
     * @note Call set_discovered_tools() before start() to set tool names.
     */
    AmsBackendToolChanger(IMoonrakerAPI* api, helix::IMoonrakerClient* client);

    /**
     * @brief Set discovered tool names from PrinterCapabilities
     *
     * Must be called before start() to initialize tool structures.
     * Tool names are extracted from printer.objects.list (e.g., "T0", "T1").
     *
     * @param tool_names Vector of tool names
     */
    void set_discovered_tools(std::vector<std::string> tool_names) override;

    // State queries
    [[nodiscard]] AmsSystemInfo get_system_info() const override;
    [[nodiscard]] AmsType get_type() const override;
    [[nodiscard]] SlotInfo get_slot_info(int slot_index) const override;

    // Tool changers give each tool its own independent toolhead with no shared
    // physical tray/housing, so the AMS detail view's tray graphic is hidden.
    [[nodiscard]] bool has_physical_tray() const override {
        return false;
    }
    // The per-slot tool badge ("T0", "T1", ...) is redundant with the toolhead
    // label shown below each slot on a tool changer.
    [[nodiscard]] bool should_hide_slot_tool_badge() const override {
        return true;
    }
    // Marker for tool-changer expected-hardware recording during wizard setup.
    [[nodiscard]] const char* get_klipper_object_name() const override {
        return "toolchanger";
    }

    [[nodiscard]] RemapStrategy get_remap_strategy() const override {
        return RemapStrategy::Native;
    }

    /// A tool changer has one extruder per tool, so its table is identity — but it
    /// is a real table the firmware owns and ASSIGN_TOOL rewrites, and
    /// get_tool_mapping() returns it.
    [[nodiscard]] bool owns_tool_mapping_table() const override {
        return true;
    }

    /// Klipper tool-changers have one extruder per tool. Tool N sources slot N
    /// directly — identity mapping — which activates per-extruder consumption
    /// tracking in FilamentConsumptionTracker.
    [[nodiscard]] std::optional<int> slot_for_extruder(int extruder_idx) const override {
        if (extruder_idx < 0 || extruder_idx >= static_cast<int>(get_system_info().total_slots)) {
            return std::nullopt;
        }
        return extruder_idx;
    }

    // Path visualization (PARALLEL topology for tool changers)
    [[nodiscard]] PathTopology get_topology() const override;
    [[nodiscard]] PathSegment get_filament_segment() const override;
    [[nodiscard]] PathSegment get_slot_filament_segment(int slot_index) const override;
    [[nodiscard]] PathSegment infer_error_segment() const override;

  protected:
    // Operations. Gated by AmsSubscriptionBackend's NVI wrapper.
    AmsError do_load_filament(int slot_index) override;
    AmsError do_unload_filament(int slot_index) override;
    AmsError do_select_slot(int slot_index) override;

    /// On a tool changer, selecting a slot means mounting that toolhead —
    /// do_select_slot() forwards to do_change_tool(), which emits SELECT_TOOL
    /// and swaps what is on the carriage.
    [[nodiscard]] bool select_slot_moves_toolhead() const override {
        return true;
    }

    /**
     * @brief Mount a physical toolhead. The argument is a SLOT INDEX.
     *
     * Named `tool_number` only because the base signature is, and validated as a
     * slot: the implementation emits `SELECT_TOOL T={n}`, which resolves through
     * the toolchanger's own list and therefore BYPASSES ASSIGN_TOOL remapping.
     * That is deliberate — a tap on lane 2 must mount the toolhead the user
     * tapped, not whichever physical tool the slicer's T2 was reassigned to.
     *
     * So on this backend a tool number and a slot index are NOT
     * interchangeable, and anything that has to cross between them goes through
     * the tool map (AmsSystemInfo::tool_to_slot_map / SlotInfo::mapped_tool),
     * never by assuming they are equal.
     */
    AmsError do_change_tool(int tool_number) override;

  public:
    /**
     * @brief Offer Unload only for the tool currently on the carriage.
     *
     * The base rule's PARALLEL arm keys on is_present(), which holds for every
     * slot on this backend forever — a slot here is a physical toolhead, never
     * EMPTY or UNKNOWN. That read true everywhere, so the context menu offered
     * Unload (UNSELECT_TOOL) on tools parked in their docks and, through
     * decide_can_load()'s inverted `!toolhead_unload` factor, kept Load disabled
     * on all of them (prestonbrown/helixscreen#1199).
     *
     * A klipper-toolchanger carries exactly one tool at a time, and unload_filament()
     * is an unmount rather than a filament retraction, so `toolchanger.tool_number`
     * is the whole answer. Load stays offered on every docked tool, which is what
     * mounting one means here.
     */
    [[nodiscard]] bool can_unload_from_toolhead(int slot_index) const override;

    /**
     * @brief "Seated and loaded" is "mounted on the carriage" here — same answer
     *        as can_unload_from_toolhead().
     *
     * The base rule (current_slot + filament_loaded) would agree on this backend
     * in the settled case, because both fields are written from one tool number,
     * but it cannot see a dock-sensor fault: apply_tool_sensor_locked() holds the
     * last known tool through one on purpose, so the pair keeps naming a
     * perfectly plausible carriage tool while the sensors say they cannot tell.
     *
     * That matters because the affordance gate is an OR. AmsContextMenu combines
     * the snapshot it took at open (can_unload_from_toolhead) with the live
     * accessors — `pending_is_loaded_ || slot_is_actively_loaded(i) ||
     * slot_has_filament_at_toolhead(i)` — so a withdrawal by one of them is
     * restored by any other that still reads true. Both questions therefore
     * resolve through the same private rule, and the menu needs no knowledge of
     * dock sensors to get the right answer.
     */
    [[nodiscard]] bool slot_is_actively_loaded(int slot_index) const override;

    /// Load is SELECT_TOOL and unload is UNSELECT_TOOL: a mount and an unmount,
    /// with no filament motion of any kind. See do_load_filament().
    [[nodiscard]] bool load_mounts_tool() const override {
        return true;
    }

    /// A swap is SELECTING for its whole duration on a controller that names no
    /// direction, and UNLOADING then SELECTING on one that does. Nothing heats,
    /// feeds or purges, so the filament vocabulary never saw the operation at
    /// all - neither its start nor, half way through, its continuation.
    [[nodiscard]] bool action_tracks_step_operation(AmsAction action) const override {
        // HEATING is here despite a changer never heating: it is the optimistic
        // marker AmsOperationSidebar::start_operation() sets the moment a user
        // starts an operation, before any frame arrives. Omitting it made the
        // sidebar hide the bar and clear target_load_slot_ on our OWN dispatch,
        // after which the rest of a user-initiated swap read as externally
        // started.
        return action == AmsAction::SELECTING || action == AmsAction::UNLOADING ||
               action == AmsAction::HEATING;
    }

    /// Explains the one refusal a user can do something about: the dock sensors
    /// cannot say which tool is on the head, so every slot's Unmount is
    /// disabled and none of them look like the reason.
    [[nodiscard]] std::string unload_blocked_reason(int slot_index) const override;

    /// Step bar for a swap, built from what THIS machine actually reports.
    ///
    /// A hotend changer's phases are the add-on's `operation` (dropping/picking)
    /// and, where the controller publishes it, the frame-side gripper opening
    /// and closing around them. A changer with neither - plain
    /// klipper-toolchanger, or MedusaHC stock, whose only signal is
    /// `toolchanger.status == "changing"` - returns a SUPPRESSED model: it has no
    /// phases to show, and the generic Heat/Feed/Purge fallback describes a
    /// filament system rather than a tool changer.
    [[nodiscard]] OperationStepModel get_operation_step_model(StepOperationType op) const override;

    /// The shared operation-phase subject. apply_tool_sensor_locked() writes the
    /// step index into system_info_.operation_phase, the same route the U1 uses.
    [[nodiscard]] lv_subject_t* get_operation_step_index_subject(StepOperationType op) override;

    // NOTE: has_per_slot_loaded_authority() is deliberately NOT overridden, and
    // this backend has a reason none of the others do: it carries no filament
    // signal at all. get_slot_filament_segment() returns NOZZLE unconditionally,
    // no per-tool switch is read, and is_filament_loaded() is just
    // `tool_number >= 0`. The only fact the parse can state is which tool is on
    // the carriage — single-valued, which is exactly what the aggregate
    // current_slot + filament_loaded pair encodes, derived from
    // klipper-toolchanger's own toolchanger.tool_number — through the forward
    // tool map, since that number is the ASSIGNED one and ASSIGN_TOOL can point
    // it at any physical tool. The per-slot LOADED
    // stamp is derived FROM that pair (refresh_slot_statuses_locked), so
    // believing it back would only add staleness — the same argument that keeps
    // Happy Hare on the aggregate rule (prestonbrown/helixscreen#1199).
    //
    // In particular `tool <name>.mounted` is NOT that authority: it arrives on a
    // separate Moonraker object, and an all-tools-mounted payload is a shape we
    // emit ourselves in mock mode.
    //
    // slot_is_actively_loaded() IS overridden, which is the other route the base
    // offers ("backends may still override outright where neither rule fits").
    // The aggregate rule is right about which tool is seated and blind to the
    // dock-sensor fault that says the seat cannot be read at all.

    // Recovery
    AmsError recover() override;
    AmsError reset() override;
    AmsError cancel() override;

    // Configuration
    AmsError set_slot_info(int slot_index, const SlotInfo& info, bool persist = true) override;
    AmsError set_tool_mapping_impl(int tool_number, int slot_index) override;

    // Tool mapping via klipper-toolchanger ASSIGN_TOOL command
    AmsError reset_tool_mappings() override;
    [[nodiscard]] std::vector<int> get_tool_mapping() const override;

    // Bypass mode (not applicable for tool changers)
    AmsError enable_bypass() override;
    AmsError disable_bypass() override;
    [[nodiscard]] bool is_bypass_active() const override;

    /**
     * @brief Declare the filament feeder this machine exposes, if any
     *
     * Resolved from discovery by toolchanger_addon::resolve_feeder() and handed over at
     * construction. Absent by default, so a tool changer nobody told anything
     * exposes no device actions -- which is every klipper-toolchanger build
     * that swaps a whole toolhead.
     */
    void set_feeder(helix::toolchanger_addon::Feeder feeder) override {
        feeder_ = std::move(feeder);
    }

    /// Dock-sensor reader. When set, its answer overrides toolchanger.tool_number.
    void set_tool_sensor(helix::toolchanger_addon::ToolSensor sensor) override {
        tool_sensor_ = std::move(sensor);
    }

    /// Swap commands for a machine without klipper-toolchanger. Absent leaves
    /// SELECT_TOOL/UNSELECT_TOOL in place.
    void set_tool_commands(helix::toolchanger_addon::ToolCommands commands) override {
        tool_commands_ = std::move(commands);
    }

    // Device Actions -- the feeder, when the machine has one.
    [[nodiscard]] std::vector<helix::printer::DeviceSection> get_device_sections() const override;
    [[nodiscard]] std::vector<helix::printer::DeviceAction> get_device_actions() const override;
    AmsError execute_device_action(const std::string& action_id,
                                   const std::any& value = {}) override;

  protected:
    // Allow test helper access to private members
    friend class ToolChangerCharHelper;
    friend class ToolChangerTestAccess;

    // --- AmsSubscriptionBackend hooks ---
    AmsError additional_start_checks() override;

    /// Post-start work. Loads the slot-override store here and NOT in
    /// additional_start_checks(), which start() calls with mutex_ held.
    void on_started() override;
    void handle_status_update(const nlohmann::json& notification) override;
    const char* backend_log_tag() const override {
        return "[AMS ToolChanger]";
    }

    /// dispatch_operation() sets the optimistic action (begin_dispatch_locked)
    /// BEFORE calling ensure_homed_then() -- on decline, the base class's
    /// generic IDLE reset alone leaves pending_dispatch_action_ armed and
    /// operation_detail stale, so route through abandon_dispatch() instead,
    /// the same unwind dispatch_operation()'s own `if (!result)` net uses.
    /// ToolChanger has no stuck-action watchdog at all, so this matters even
    /// more here than on AFC.
    void on_home_confirmation_declined() override;

  private:
    /// Feeder this machine exposes; absent unless set_feeder() said otherwise.
    helix::toolchanger_addon::Feeder feeder_;
    /// Absent on every tool changer without dock sensors.
    helix::toolchanger_addon::ToolSensor tool_sensor_;
    /// Absent whenever klipper-toolchanger owns the swap.
    helix::toolchanger_addon::ToolCommands tool_commands_;
    /// Latest per-dock occupancy from the dock sensors, indexed by slot: true
    /// seated, false empty, nullopt never reported. Kept across frames, because
    /// Moonraker republishes only what CHANGED and a frame carrying just the
    /// tool number says nothing about the docks.
    std::vector<std::optional<bool>> dock_seated_;
    /// Frame-side gripper, and whether this machine reports it AT ALL. The
    /// second flag is what get_operation_step_model() keys on: a controller that
    /// publishes feeder_open earns the release/grip steps, and a machine that
    /// never mentions it gets a bar with only the phases it can actually drive
    /// rather than two steps that would sit grey forever. Latched once true -
    /// Moonraker republishes only changed fields, so a later frame omitting
    /// feeder_open is not the machine retracting the capability.
    bool feeder_open_ = false;
    bool feeder_state_reported_ = false;
    /// Dock sensors cannot say what is on the head. Latched rather than inferred
    /// from current_slot, because the sensor-error path deliberately HOLDS the
    /// last known tool: current_slot stays >= 0 through the fault and names a
    /// tool that may not be there.
    bool sensor_error_ = false;
    /// The gripper has been open at some point during the operation currently
    /// running. Cleared when it ends.
    ///
    /// Without this, idle-with-the-gripper-closed is ambiguous in a THIRD way:
    /// it is the resting state, it is the closing grip at the end of a swap, and
    /// it is also every frame between dispatching SELECT_TOOL and the machine
    /// actually moving. Treating that last one as the closing grip paints the
    /// bar complete for the second before the swap starts. The closing grip is
    /// only recognisable once the gripper has actually opened.
    bool feeder_opened_this_operation_ = false;
    /// Whether this machine's phase word names the swap DIRECTION. Latched from
    /// ToolReading::phase_names_direction, which is answerable from the first
    /// status frame - the phase WORDS are not, since "picking" only appears once
    /// a swap is already running and the step bar has to be built before that.
    bool direction_reported_ = false;
    /// Whether a real non-idle operation frame ("dropping"/"picking"/"changing")
    /// has been seen since the idle-with-gripper-open hold in
    /// apply_tool_sensor_locked() last released. That hold exists because a
    /// swap's own FIRST frame is idle-with-the-gripper-open, but its condition
    /// is partly derived from the action the hold itself sets - so without a
    /// separate signal, a machine that settles into a LATER idle-with-open
    /// frame (e.g. an unmount that ends with the head empty and nothing to
    /// re-grip) would keep re-satisfying the same condition forever and never
    /// reach IDLE again. Bounding the hold with this flag means it can only
    /// ever catch the leading idle-with-open frame: once a real operation
    /// frame has confirmed the swap is actually running, a SUBSEQUENT idle
    /// frame means it has settled, not recurred, and must be believed.
    /// Reset to false when the hold releases into IDLE, and again at the start
    /// of every fresh dispatch (begin_dispatch_locked()) in case the prior
    /// swap reached IDLE through a path that bypassed the idle branch below
    /// (e.g. parse_toolchanger_state() alone, on a frame with no addon data).
    bool operation_confirmed_ = false;

    /// Snapshot of feeder_state_reported_ / direction_reported_ taken the last
    /// time get_operation_step_model() built a sequence. step_index_for_phase_locked()
    /// resolves its index against this SAME snapshot, not whatever the latches
    /// read right now: the model is captured once (at operation start, by the
    /// sidebar) while the index is recomputed on every frame, and Moonraker
    /// republishes only the fields that CHANGED. A latch that flips mid-
    /// operation without the model being rebuilt would otherwise put the index
    /// computation against a longer/different sequence than the one actually
    /// on screen. get_operation_step_model() is const, so these are mutable.
    ///
    /// step_model_captured_ stays false until the first call: nothing has
    /// pinned a sequence yet, so step_index_for_phase_locked() falls back to
    /// the LIVE latches rather than the (false, false) power-on default, which
    /// would otherwise build an always-empty sequence for any caller that asks
    /// for the phase without ever having asked for the model first.
    mutable bool step_model_captured_ = false;
    mutable bool step_model_feeder_reported_ = false;
    mutable bool step_model_direction_reported_ = false;

    /**
     * @brief Parse toolchanger state from Moonraker JSON
     *
     * Extracts toolchanger object and updates system_info_.
     *
     * @param tc_data JSON object containing toolchanger data
     */
    void parse_toolchanger_state(const nlohmann::json& tc_data);

    /// Apply an add-on dock-sensor reading over the toolchanger's own claim.
    /// Caller holds mutex_.
    void apply_tool_sensor_locked(const helix::toolchanger_addon::ToolReading& reading);

    /// Is this slot's toolhead on the carriage, as far as anything can tell?
    /// The one rule behind can_unload_from_toolhead() and
    /// slot_is_actively_loaded(), which ask it for different reasons and must
    /// not be able to disagree. Caller holds mutex_.
    [[nodiscard]] bool slot_is_mounted_locked(int slot_index) const;

    /// Step index for `operation` under the model this machine gets, or -1 when
    /// the phase does not map to a step. `mid_operation` says whether a swap was
    /// running when the frame arrived, which is the only thing separating the
    /// closing grip of one from the resting closed gripper. Caller holds mutex_.
    int step_index_for_phase_locked(const std::string& operation, bool mid_operation) const;

    /// Layer the user's stored spool metadata over a slot. Caller holds mutex_.
    ///
    /// Unlike every other backend that does this, there is nothing underneath:
    /// parse_tool_state() reads `mounted` and `active` and nothing else, so
    /// klipper-toolchanger reports no material, colour, brand or weight at all.
    /// The store is the SOLE source of filament identity here, not a layer over
    /// a firmware reading, and initialize_tools() resets colour to default grey
    /// on every rediscovery - which is exactly what used to wipe the user's edit.
    void apply_overrides(SlotInfo& slot, int slot_index);

    /// Per-slot user metadata, keyed by slot index. Written and read only under
    /// mutex_.
    std::unordered_map<int, helix::ams::FilamentSlotOverride> overrides_;

    /// Moonraker-DB-backed store. Null until additional_start_checks() builds it
    /// (needs api_), and on backends constructed without an API in tests.
    std::unique_ptr<helix::ams::FilamentSlotOverrideStore> override_store_;

    /**
     * @brief Parse individual tool state from Moonraker JSON
     *
     * Updates the slot corresponding to this tool.
     *
     * @param tool_name Tool name (e.g., "T0")
     * @param tool_data JSON object containing tool data
     */
    void parse_tool_state(const std::string& tool_name, const nlohmann::json& tool_data);

    /**
     * @brief Re-derive every slot's LOADED stamp from the carriage tool.
     *
     * Called from both parse paths so the per-slot status and the aggregate pair
     * are always computed from the same value and cannot disagree. Only
     * AVAILABLE and LOADED occur on this backend — a toolhead is always present.
     *
     * @note Must be called while holding mutex_.
     */
    void refresh_slot_statuses_locked();

    /**
     * @brief Convert toolchanger status string to AmsAction
     *
     * @param status Status string from toolchanger.status
     * @return Corresponding AmsAction enum value
     */
    static AmsAction status_to_action(const std::string& status);

    /**
     * @brief Initialize tool structures based on discovered tool names
     *
     * Creates SlotInfo entries for each tool.
     */
    void initialize_tools();

    /**
     * @brief Find slot index for a tool name
     *
     * @param tool_name Tool name (e.g., "T0", "T1")
     * @return Slot index or -1 if not found
     */
    [[nodiscard]] int find_slot_for_tool(const std::string& tool_name) const;

    /**
     * @brief Validate slot index is within range
     *
     * @param slot_index Slot index to validate
     * @return AmsError (SUCCESS if valid, INVALID_SLOT otherwise)
     */
    AmsError validate_slot_index(int slot_index) const;

    // === Optimistic dispatch + macro-ack resolution =========================
    //
    // The only thing that can move the action off the value a dispatch sets is
    // a `toolchanger.status` frame, and Moonraker only pushes fields whose value
    // CHANGED. klipper-toolchanger short-circuits SELECT_TOOL on the tool already
    // on the carriage ("Tool tool T4 already selected") without touching
    // `status`, so a no-op produces no frame at all and the action never leaves
    // SELECTING — the filament panel spins and is_busy() locks out every later
    // operation until restart. Same defect and same fix as AFC (#1183): set the
    // action optimistically so there is a transition to complete, and resolve it
    // on the macro's own gcode ack when the toolchanger never claimed the op.

    /// Action the most recent dispatch set optimistically, cleared once that
    /// dispatch has been resolved — by its ack, by klipper-toolchanger taking
    /// the operation over, or by a newer dispatch superseding it.
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

    /// Resolve a dispatch to IDLE on its macro's gcode ack, but only when the
    /// toolchanger never took the operation over. Main thread; must NOT hold
    /// mutex_.
    void finalize_dispatch_after_macro(uint64_t generation);

    /// Send a tool operation: set @p action optimistically, dispatch @p gcode
    /// through ensure_homed_then(), and resolve on the macro's ack. Caller must
    /// NOT hold mutex_ and must have passed check_preconditions().
    AmsError dispatch_operation(std::string gcode, AmsAction action);

    // Tool changer specific state
    std::vector<std::string> tool_names_; ///< Tool names from discovery

    // Cached toolchanger state
    bool tools_initialized_{false}; ///< Have we received initial state?

    // Per-tool mounted state (for quick lookup)
    std::vector<bool> tool_mounted_; ///< Which tools are mounted
};
