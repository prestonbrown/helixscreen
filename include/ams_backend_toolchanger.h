// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_subscription_backend.h"

#include <cstdint>
#include <optional>
#include <string>
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

    // Recovery
    AmsError recover() override;
    AmsError reset() override;
    AmsError cancel() override;

    // Configuration
    AmsError set_slot_info(int slot_index, const SlotInfo& info, bool persist = true) override;
    AmsError set_tool_mapping(int tool_number, int slot_index) override;

    // Tool mapping via klipper-toolchanger ASSIGN_TOOL command
    AmsError reset_tool_mappings() override;
    [[nodiscard]] helix::printer::ToolMappingCapabilities
    get_tool_mapping_capabilities() const override;
    [[nodiscard]] std::vector<int> get_tool_mapping() const override;

    // Bypass mode (not applicable for tool changers)
    AmsError enable_bypass() override;
    AmsError disable_bypass() override;
    [[nodiscard]] bool is_bypass_active() const override;

    // Device Actions (stub - not applicable for tool changers)
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
    /**
     * @brief Parse toolchanger state from Moonraker JSON
     *
     * Extracts toolchanger object and updates system_info_.
     *
     * @param tc_data JSON object containing toolchanger data
     */
    void parse_toolchanger_state(const nlohmann::json& tc_data);

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
