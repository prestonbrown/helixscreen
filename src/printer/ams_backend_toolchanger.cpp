// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_toolchanger.h"

#include "ams_error.h"
#include "ams_tool_map_sync.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <sstream>
#include <utility>

using namespace helix;

// ============================================================================
// Construction / Destruction
// ============================================================================

AmsBackendToolChanger::AmsBackendToolChanger(IMoonrakerAPI* api, IMoonrakerClient* client)
    : AmsSubscriptionBackend(api, client) {
    // Initialize system info with tool changer defaults
    system_info_.type = AmsType::TOOL_CHANGER;
    system_info_.type_name = "Tool Changer";

    // Tool changer capabilities
    system_info_.supports_tool_mapping = true; // Via klipper-toolchanger ASSIGN_TOOL
    system_info_.supports_bypass = false;      // No bypass on tool changers
    system_info_.has_hardware_bypass_sensor = false;

    spdlog::debug("[AMS ToolChanger] Backend created");
}

void AmsBackendToolChanger::set_discovered_tools(std::vector<std::string> tool_names) {
    std::lock_guard<std::mutex> lock(mutex_);

    tool_names_ = std::move(tool_names);

    // Initialize tool structures now that we have tool names
    if (!tool_names_.empty()) {
        initialize_tools();
    }

    spdlog::info("[AMS ToolChanger] Set {} discovered tools", tool_names_.size());
}

// Destructor not needed -- base class handles subscription cleanup

// ============================================================================
// Lifecycle Management
// ============================================================================

AmsError AmsBackendToolChanger::additional_start_checks() {
    if (tool_names_.empty()) {
        spdlog::error("[AMS ToolChanger] Cannot start: No tools discovered. "
                      "Call set_discovered_tools() before start()");
        return AmsErrorHelper::not_connected("No tools discovered");
    }
    return AmsErrorHelper::success();
}

// stop(), release_subscriptions(), is_running() provided by AmsSubscriptionBackend

// ============================================================================
// Event System
// ============================================================================

// set_event_callback() and emit_event() provided by AmsSubscriptionBackend

// ============================================================================
// State Queries
// ============================================================================

AmsSystemInfo AmsBackendToolChanger::get_system_info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_;
}

AmsType AmsBackendToolChanger::get_type() const {
    return AmsType::TOOL_CHANGER;
}

SlotInfo AmsBackendToolChanger::get_slot_info(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);

    const auto* slot = system_info_.get_slot_global(slot_index);
    if (slot) {
        return *slot;
    }

    // Return empty slot info for invalid index
    SlotInfo empty;
    empty.slot_index = -1;
    return empty;
}

// get_current_action(), get_current_tool(), get_current_slot(), is_filament_loaded()
// provided by AmsSubscriptionBackend

bool AmsBackendToolChanger::can_unload_from_toolhead(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);

    // One tool on the carriage at a time, and "unload" here means UNSELECT_TOOL —
    // an unmount, not a filament retraction. Unmounting a docked tool is
    // meaningless, so only the carriage tool qualifies. See the header note for
    // why the inherited is_present() rule cannot express this.
    if (slot_index < 0 || slot_index >= system_info_.total_slots) {
        return false;
    }
    // Against current_SLOT, not current_tool. On this backend the two are
    // deliberately not interchangeable: ASSIGN_TOOL moves a G-code tool number
    // onto a different physical tool, and klipper-toolchanger reports the
    // ASSIGNED number in toolchanger.tool_number. Comparing a slot index to it
    // offered Unload on the wrong toolhead whenever a remap was in effect.
    return system_info_.current_slot >= 0 && slot_index == system_info_.current_slot;
}

// ============================================================================
// Path Visualization
// ============================================================================

PathTopology AmsBackendToolChanger::get_topology() const {
    return PathTopology::PARALLEL; // Each tool has its own independent path
}

PathSegment AmsBackendToolChanger::get_filament_segment() const {
    std::lock_guard<std::mutex> lock(mutex_);

    // For tool changers, filament segment depends on whether a tool is mounted
    if (system_info_.current_tool >= 0 && system_info_.filament_loaded) {
        return PathSegment::NOZZLE; // Tool is mounted and active
    }
    return PathSegment::SPOOL; // No tool mounted (all tools in docks)
}

PathSegment AmsBackendToolChanger::get_slot_filament_segment(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (slot_index < 0 || slot_index >= static_cast<int>(tool_mounted_.size())) {
        return PathSegment::NONE;
    }

    // For tool changers, each slot is a complete tool with filament loaded
    // through the nozzle — both mounted and docked tools have filament at nozzle
    return PathSegment::NOZZLE;
}

PathSegment AmsBackendToolChanger::infer_error_segment() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (system_info_.action == AmsAction::ERROR) {
        // For tool changers, errors typically occur at the dock or carriage
        return PathSegment::HUB; // Use HUB to represent the docking area
    }
    return PathSegment::NONE;
}

// ============================================================================
// Moonraker Status Update Handling
// ============================================================================

void AmsBackendToolChanger::handle_status_update(const nlohmann::json& notification) {
    // notify_status_update has format: { "method": "notify_status_update", "params": [{ ... },
    // timestamp] }
    if (!notification.contains("params") || !notification["params"].is_array() ||
        notification["params"].empty()) {
        return;
    }

    const auto& params = notification["params"][0];
    if (!params.is_object()) {
        return;
    }

    bool state_changed = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Check for toolchanger object updates
        if (params.contains("toolchanger")) {
            const auto& tc_data = params["toolchanger"];
            if (tc_data.is_object()) {
                spdlog::trace("[AMS ToolChanger] Received toolchanger status update");
                parse_toolchanger_state(tc_data);
                state_changed = true;
            }
        }

        // Check for individual tool updates (e.g., "tool T0", "tool T1")
        for (const auto& tool_name : tool_names_) {
            std::string key = "tool " + tool_name;
            if (params.contains(key)) {
                const auto& tool_data = params[key];
                if (tool_data.is_object()) {
                    spdlog::trace("[AMS ToolChanger] Received {} status update", key);
                    parse_tool_state(tool_name, tool_data);
                    state_changed = true;
                }
            }
        }
    }

    // Emit event OUTSIDE the lock to avoid deadlock if the callback
    // queries backend state (e.g., calls get_system_info() which acquires mutex_)
    if (state_changed) {
        emit_event(EVENT_STATE_CHANGED);
    }
}

void AmsBackendToolChanger::parse_toolchanger_state(const nlohmann::json& tc_data) {
    // Parse status: toolchanger.status
    // Values: "ready", "changing", "error", "uninitialized"
    if (tc_data.contains("status") && tc_data["status"].is_string()) {
        std::string status_str = tc_data["status"].get<std::string>();
        system_info_.action = status_to_action(status_str);
        system_info_.operation_detail = status_str;
        spdlog::trace("[AMS ToolChanger] Status: {} -> {}", status_str,
                      ams_action_to_string(system_info_.action));

        // The toolchanger reporting anything but "ready" means it has taken the
        // dispatched operation over, so its own state machine owns completion
        // from here and the pending macro ack must keep its hands off — a real
        // SELECT_TOOL acks when the macro returns, which is well before the
        // carriage finishes, and forcing IDLE there would report done mid-swap.
        // A re-echoed "ready" is deliberately NOT taking over: that is the
        // no-op case this mechanism exists to catch (#1183). It is harmless
        // anyway — the frame already moved the action to IDLE, so the ack's own
        // value guard drops it.
        if (pending_dispatch_action_.has_value() && system_info_.action != AmsAction::IDLE) {
            spdlog::debug("[AMS ToolChanger] Toolchanger took over the dispatched operation "
                          "({} from status '{}')",
                          ams_action_to_string(system_info_.action), status_str);
            pending_dispatch_action_.reset();
        }
    }

    // Parse current tool: toolchanger.tool_number
    // -1 = no tool selected
    if (tc_data.contains("tool_number") && tc_data["tool_number"].is_number_integer()) {
        int tool_num = tc_data["tool_number"].get<int>();
        system_info_.current_tool = tool_num;
        // toolchanger.tool_number is the ASSIGNED G-code number of the carriage
        // tool, which ASSIGN_TOOL can move onto any physical tool — it is only
        // the slot index while the mapping is identity. Resolve it through the
        // forward map so the carriage lane is right under a remap too; an
        // unmapped number falls back to itself, the pre-remap answer.
        int seated_slot = -1;
        if (tool_num >= 0) {
            seated_slot = tool_num < static_cast<int>(system_info_.tool_to_slot_map.size())
                              ? system_info_.tool_to_slot_map[static_cast<size_t>(tool_num)]
                              : -1;
            if (seated_slot < 0) {
                seated_slot = tool_num;
            }
        }
        system_info_.current_slot = seated_slot;
        system_info_.filament_loaded = (tool_num >= 0);
        refresh_slot_statuses_locked();
        spdlog::trace("[AMS ToolChanger] Current tool: T{} (physical slot {})", tool_num,
                      seated_slot);
    }

    // Parse tool list: toolchanger.tool_numbers and toolchanger.tool_names
    // This can be used to dynamically update the tool list
    if (tc_data.contains("tool_numbers") && tc_data["tool_numbers"].is_array()) {
        spdlog::trace("[AMS ToolChanger] Tool numbers: {}", tc_data["tool_numbers"].dump());
    }
}

void AmsBackendToolChanger::parse_tool_state(const std::string& tool_name,
                                             const nlohmann::json& tool_data) {
    int slot_idx = find_slot_for_tool(tool_name);
    if (slot_idx < 0) {
        spdlog::warn("[AMS ToolChanger] Unknown tool: {}", tool_name);
        return;
    }

    // Parse mounted state: tool.mounted
    //
    // Recorded, but deliberately NOT the source of the slot's LOADED stamp. This
    // is a different Moonraker object from the toolchanger one that assigns the
    // aggregate pair, and an all-tools-mounted payload is a shape we emit
    // ourselves in mock mode — writing `mounted ? LOADED : AVAILABLE` straight
    // into slot.status marked every tool loaded at once (#1199). The stamp is
    // re-derived from the carriage tool instead, so the two writers agree.
    if (tool_data.contains("mounted") && tool_data["mounted"].is_boolean()) {
        bool mounted = tool_data["mounted"].get<bool>();
        if (slot_idx < static_cast<int>(tool_mounted_.size())) {
            tool_mounted_[slot_idx] = mounted;
        }
        refresh_slot_statuses_locked();
        spdlog::trace("[AMS ToolChanger] Tool {} mounted: {}", tool_name, mounted);
    }

    // Parse active state: tool.active
    if (tool_data.contains("active") && tool_data["active"].is_boolean()) {
        bool active = tool_data["active"].get<bool>();
        spdlog::trace("[AMS ToolChanger] Tool {} active: {}", tool_name, active);
    }

    // Parse offsets (stored but not currently used in SlotInfo)
    if (tool_data.contains("gcode_x_offset") || tool_data.contains("gcode_y_offset") ||
        tool_data.contains("gcode_z_offset")) {
        spdlog::trace("[AMS ToolChanger] Tool {} has offset data", tool_name);
    }
}

void AmsBackendToolChanger::refresh_slot_statuses_locked() {
    if (system_info_.units.empty()) {
        return;
    }

    // A toolhead is always physically there, so EMPTY/UNKNOWN never occur here:
    // the stamp is a straight two-way split on the carriage tool.
    auto& slots = system_info_.units[0].slots;
    for (int i = 0; i < static_cast<int>(slots.size()); ++i) {
        // current_slot, not current_tool — `i` indexes physical toolheads, and
        // under an ASSIGN_TOOL remap the carriage tool's G-code number is not
        // its slot index (see the tool_number parse). The old comparison
        // stamped LOADED on the lane that merely shares an index with the
        // number.
        slots[i].status = (system_info_.current_slot >= 0 && i == system_info_.current_slot)
                              ? SlotStatus::LOADED
                              : SlotStatus::AVAILABLE;
    }
}

AmsAction AmsBackendToolChanger::status_to_action(const std::string& status) {
    if (status == "ready") {
        return AmsAction::IDLE;
    }
    if (status == "changing") {
        return AmsAction::SELECTING;
    }
    if (status == "error") {
        return AmsAction::ERROR;
    }
    if (status == "uninitialized") {
        return AmsAction::RESETTING;
    }
    return AmsAction::IDLE;
}

void AmsBackendToolChanger::initialize_tools() {
    int tool_count = static_cast<int>(tool_names_.size());

    AmsUnit unit;
    unit.unit_index = 0;
    unit.name = "Tool Changer";
    unit.slot_count = tool_count;
    unit.first_slot_global_index = 0;
    unit.connected = true;
    unit.has_encoder = false;
    unit.has_toolhead_sensor = false;
    unit.has_slot_sensors = false;

    // Initialize slots for each tool
    tool_mounted_.clear();
    tool_mounted_.resize(tool_count, false);

    for (int i = 0; i < tool_count; ++i) {
        SlotInfo slot;
        slot.slot_index = i;
        slot.global_index = i;
        slot.status = SlotStatus::AVAILABLE; // Tools start as available (docked)
        slot.color_rgb = AMS_DEFAULT_SLOT_COLOR;
        slot.spool_name = tool_names_[i]; // Use tool name as placeholder

        unit.slots.push_back(slot);
    }

    system_info_.units.clear();
    system_info_.units.push_back(unit);
    system_info_.total_slots = tool_count;

    // Initialize tool-to-slot mapping (1:1 for tool changers) in BOTH
    // directions from one pass: the slot loop above deliberately leaves
    // mapped_tool alone so this is the only writer. ASSIGN_TOOL can move a
    // G-code tool number onto a different physical tool later; when it does,
    // the badge (mapped_tool) and the op-button lane (tool_to_slot_map) have to
    // move together or they name different toolheads.
    system_info_.tool_to_slot_map.clear();
    helix::printer::sync_tool_map_from_forward(system_info_, /*identity_fallback=*/true);

    // A toolchanger frame may already have named the carriage tool before the
    // tool list arrived; re-derive so the fresh slots match it.
    refresh_slot_statuses_locked();

    tools_initialized_ = true;
    spdlog::info("[AMS ToolChanger] Initialized {} tools", tool_count);
}

int AmsBackendToolChanger::find_slot_for_tool(const std::string& tool_name) const {
    auto it = std::find(tool_names_.begin(), tool_names_.end(), tool_name);
    if (it != tool_names_.end()) {
        return static_cast<int>(std::distance(tool_names_.begin(), it));
    }
    return -1;
}

// ============================================================================
// Operations
// ============================================================================

// NOTE: Must be called while holding mutex_ (accesses system_info_ without lock)
// check_preconditions() provided by AmsSubscriptionBackend

// NOTE: Must be called while holding mutex_ (accesses system_info_ without lock)
AmsError AmsBackendToolChanger::validate_slot_index(int slot_index) const {
    // Special case: no tools discovered
    if (system_info_.total_slots == 0) {
        return AmsErrorHelper::not_connected("No tools discovered");
    }
    if (slot_index < 0 || slot_index >= system_info_.total_slots) {
        return AmsErrorHelper::invalid_slot(slot_index, system_info_.total_slots - 1);
    }
    return AmsErrorHelper::success();
}

// execute_gcode() provided by AmsSubscriptionBackend

// ============================================================================
// Optimistic dispatch + macro-ack resolution (#1183)
// ============================================================================

uint64_t AmsBackendToolChanger::begin_dispatch_locked(AmsAction action) {
    // A newer dispatch supersedes any older one whose ack is still in flight:
    // that ack presents a stale generation and finalize_dispatch_after_macro()
    // drops it, so it can never resolve the operation now running.
    const uint64_t generation = ++dispatch_generation_;
    pending_dispatch_action_ = action;

    system_info_.action = action;
    // Otherwise the sidebar keeps showing the previous operation's detail (the
    // raw toolchanger status string) until the next frame, which for a no-op
    // never comes.
    system_info_.operation_detail =
        (action == AmsAction::UNLOADING) ? lv_tr("Unloading") : lv_tr("Tool swap");

    spdlog::debug("[AMS ToolChanger] Dispatch #{}: action set optimistically to {}", generation,
                  ams_action_to_string(action));
    return generation;
}

void AmsBackendToolChanger::on_home_confirmation_declined() {
    // The confirmation modal is exclusive -- nothing else can begin a new
    // dispatch while it's up -- so the pending dispatch is always the one
    // that just prompted; that exclusivity is what makes this call correct,
    // not the generation compare inside abandon_dispatch(). abandon_dispatch()
    // takes an explicit generation to share its guard with dispatch_operation()'s
    // own failure path, which captures a real, independent value before this
    // exclusivity window even opens. Here there is no such independent
    // capture: the value handed in is dispatch_generation_ itself, so the
    // compare is trivially true and abandon_dispatch() always proceeds when a
    // dispatch is pending. Read it under mutex_ rather than as a bare member
    // access (every write to dispatch_generation_ is mutex_-guarded, in
    // begin_dispatch_locked()) so this stays race-free even though nothing
    // can invalidate it today. abandon_dispatch() already emits
    // EVENT_STATE_CHANGED, so skip the base class's default entirely.
    //
    // If this hook ever gains a non-modal caller, this guard alone will not
    // protect a genuinely newer dispatch from being abandoned -- that would
    // need the generation captured at prompt time and threaded through here
    // instead of re-read live.
    uint64_t generation;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        generation = dispatch_generation_;
    }
    abandon_dispatch(generation);
}

void AmsBackendToolChanger::abandon_dispatch(uint64_t generation) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != dispatch_generation_ || !pending_dispatch_action_.has_value()) {
            return;
        }
        pending_dispatch_action_.reset();
        system_info_.action = AmsAction::IDLE;
        system_info_.operation_detail.clear();
    }
    emit_event(EVENT_STATE_CHANGED);
}

void AmsBackendToolChanger::finalize_dispatch_after_macro(uint64_t generation) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // "Is the action still mine?" — three ways it can be someone else's:
        //   * a newer dispatch bumped the generation;
        //   * the toolchanger reported a busy status, so parse_toolchanger_state()
        //     handed the operation to klipper-toolchanger's own state machine and
        //     cleared the pending mark;
        //   * something moved the action off the value this dispatch set — a
        //     "ready" frame (which already produced the transition the UI needed)
        //     or an "error" one.
        // In all three the operation is already resolved or is still legitimately
        // running, and forcing IDLE would either lie or truncate it.
        if (generation != dispatch_generation_ || !pending_dispatch_action_.has_value() ||
            system_info_.action != *pending_dispatch_action_) {
            spdlog::debug("[AMS ToolChanger] Macro ack for dispatch #{} resolves nothing (current "
                          "action {}, generation {})",
                          generation, ams_action_to_string(system_info_.action),
                          dispatch_generation_);
            return;
        }

        // The macro ran to completion and the toolchanger never reported doing
        // anything — the "tool T4 already selected" no-op. The gcode ack is the
        // only completion signal that exists for it (#1183).
        spdlog::info("[AMS ToolChanger] Macro complete (gcode ack) with no toolchanger status "
                     "change -> IDLE");
        pending_dispatch_action_.reset();
        system_info_.action = AmsAction::IDLE;
        system_info_.operation_detail.clear();
        changed = true;
    }
    if (changed) {
        emit_event(EVENT_STATE_CHANGED);
    }
}

AmsError AmsBackendToolChanger::dispatch_operation(std::string gcode, AmsAction action) {
    uint64_t generation;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        generation = begin_dispatch_locked(action);
    }
    // Publish the optimistic action immediately, and OUTSIDE mutex_: the
    // filament panel's completion observer has to see the operation start
    // before it can ever see one end, and the callback may call back in for
    // state (get_current_action() takes mutex_).
    emit_event(EVENT_STATE_CHANGED);

    auto token = lifetime_.token();
    AmsError result = ensure_homed_then(std::move(gcode), [this, token, generation]() {
        // L081 Mechanism C: the gcode ack lands on a background thread and the
        // handler writes system_info_ under mutex_. Marshal to main.
        token.defer("AmsBackendToolChanger::dispatch_macro_complete",
                    [this, generation]() { finalize_dispatch_after_macro(generation); });
    });

    if (!result) {
        // The gcode never left: no IMoonrakerAPI, or the send was refused. No ack
        // will ever arrive, so undo the optimistic action instead of leaving the
        // UI busy and every later operation locked out by is_busy().
        spdlog::warn("[AMS ToolChanger] Dispatch #{} failed to send ({}), reverting optimistic "
                     "action",
                     generation, result.technical_msg);
        abandon_dispatch(generation);
    }
    return result;
}

// ============================================================================

AmsError AmsBackendToolChanger::do_load_filament(int slot_index) {
    // For tool changers, "load filament" means "mount tool"
    return do_change_tool(slot_index);
}

AmsError AmsBackendToolChanger::do_unload_filament(int slot_index) {
    // For tool changers, "unload" means unmount a specific tool (or current if -1)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (slot_index >= 0) {
            AmsError slot_valid = validate_slot_index(slot_index);
            if (!slot_valid) {
                return slot_valid;
            }
        } else if (system_info_.current_tool < 0) {
            return AmsErrorHelper::not_loaded();
        }
    }

    // UNSELECT_TOOL has the same no-op shortcut as SELECT_TOOL — unmounting when
    // nothing is on the carriage returns without touching toolchanger.status —
    // so it needs the same optimistic-set + ack-resolution treatment (#1183).
    if (slot_index >= 0) {
        std::string cmd = "UNSELECT_TOOL T=" + std::to_string(slot_index);
        spdlog::info("[AMS ToolChanger] Unmounting tool {}: {}", slot_index, cmd);
        return dispatch_operation(std::move(cmd), AmsAction::UNLOADING);
    }

    spdlog::info("[AMS ToolChanger] Unmounting current tool");
    return dispatch_operation("UNSELECT_TOOL", AmsAction::UNLOADING);
}

AmsError AmsBackendToolChanger::do_select_slot(int slot_index) {
    // For tool changers, selecting a slot means mounting that tool
    return do_change_tool(slot_index);
}

AmsError AmsBackendToolChanger::do_change_tool(int tool_number) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        AmsError slot_valid = validate_slot_index(tool_number);
        if (!slot_valid) {
            return slot_valid;
        }
    }

    // Use SELECT_TOOL T={n} to select by tool number via the toolchanger's
    // internal lookup, bypassing any ASSIGN_TOOL T-command remapping.
    // This ensures we mount the physical tool the user tapped, not whatever
    // the slicer's T{n} command was remapped to.
    //
    // dispatch_operation() sets SELECTING before the send — which also closes
    // the race window where a second tap could arrive before Klipper's status
    // update changes the action — and resolves it on the macro's ack when the
    // toolchanger never claims the operation (#1183).
    std::string cmd = "SELECT_TOOL T=" + std::to_string(tool_number);
    spdlog::info("[AMS ToolChanger] Mounting tool {}: {}", tool_number, cmd);
    return dispatch_operation(std::move(cmd), AmsAction::SELECTING);
}

// ============================================================================
// Recovery Operations
// ============================================================================

AmsError AmsBackendToolChanger::recover() {
    spdlog::info("[AMS ToolChanger] Attempting recovery");
    // klipper-toolchanger doesn't have a dedicated recovery command
    // Try to reinitialize the toolchanger
    return execute_gcode("INITIALIZE_TOOLCHANGER");
}

AmsError AmsBackendToolChanger::reset() {
    spdlog::info("[AMS ToolChanger] Resetting toolchanger");
    return execute_gcode("INITIALIZE_TOOLCHANGER");
}

AmsError AmsBackendToolChanger::cancel() {
    spdlog::info("[AMS ToolChanger] Cancel requested (not implemented for tool changers)");
    // Tool changes typically can't be cancelled mid-operation
    return AmsErrorHelper::not_supported("Cancel");
}

// ============================================================================
// Configuration Operations
// ============================================================================

AmsError AmsBackendToolChanger::set_slot_info(int slot_index, const SlotInfo& info,
                                              bool /*persist*/) {
    int old_mapped_tool = -1;
    std::string physical_tool_name;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        AmsError slot_valid = validate_slot_index(slot_index);
        if (!slot_valid) {
            return slot_valid;
        }

        // Update local state (for UI display)
        if (!system_info_.units.empty() &&
            slot_index < static_cast<int>(system_info_.units[0].slots.size())) {
            auto& slot = system_info_.units[0].slots[slot_index];
            old_mapped_tool = slot.mapped_tool;
            slot.color_rgb = info.color_rgb;
            slot.color_name = info.color_name;
            slot.material = info.material;
            slot.brand = info.brand;
            // No override store on this backend, so this in-memory copy is the
            // only thing keeping the editor's catalog pick visible until the
            // next parse.
            slot.catalog_id = info.catalog_id;
            slot.product_name = info.product_name;
            slot.spoolman_id = info.spoolman_id;
            slot.spool_name = info.spool_name;
            slot.remaining_weight_g = info.remaining_weight_g;
            slot.total_weight_g = info.total_weight_g;

            // Tool mapping change: persist via ASSIGN_TOOL outside the lock.
            // slot.mapped_tool stores "which G-code tool number activates this physical
            // tool"; the gcode says "T<info.mapped_tool> now activates tool_names_[slot]".
            if (info.mapped_tool != old_mapped_tool && info.mapped_tool >= 0 &&
                info.mapped_tool < static_cast<int>(system_info_.tool_to_slot_map.size()) &&
                slot_index < static_cast<int>(tool_names_.size())) {
                // One pass for both directions, and it EVICTS: the tool being
                // moved leaves its old lane unmapped, and whatever tool this
                // lane previously answered to gives it up. Writing the two
                // fields by hand left both stale halves in place, so a swap
                // ended with two lanes claiming one tool number.
                helix::printer::assign_tool_slot(system_info_, info.mapped_tool, slot_index);
                physical_tool_name = tool_names_[slot_index];
            }
        }
    }

    if (!physical_tool_name.empty()) {
        spdlog::info("[AMS ToolChanger] Remap via slot edit: T{} -> physical {} (slot {})",
                     info.mapped_tool, physical_tool_name, slot_index);
        return execute_gcode(
            fmt::format("ASSIGN_TOOL TOOL={} N={}", physical_tool_name, info.mapped_tool));
    }

    return AmsErrorHelper::success();
}

AmsError AmsBackendToolChanger::set_tool_mapping(int tool_number, int slot_index) {
    // Remap G-code tool number to a different physical tool via klipper-toolchanger's
    // ASSIGN_TOOL command. This makes Klipper's T<tool_number> command activate the
    // physical tool at slot_index instead of tool_number.
    //
    // Example: set_tool_mapping(0, 2) sends "ASSIGN_TOOL TOOL=T2 N=0"
    //   → G-code "T0" now activates physical tool T2
    std::string physical_tool_name;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        int tool_count = static_cast<int>(tool_names_.size());
        if (tool_number < 0 || tool_number >= tool_count) {
            return AmsError(AmsResult::INVALID_TOOL,
                            "Tool " + std::to_string(tool_number) + " out of range",
                            "Invalid tool number", "");
        }
        if (slot_index < 0 || slot_index >= tool_count) {
            return AmsErrorHelper::invalid_slot(slot_index, tool_count - 1);
        }

        // The physical tool to assign (slot_index maps to tool_names_[slot_index])
        physical_tool_name = tool_names_[slot_index];

        // Update internal mapping — both directions, with eviction. The AMS
        // panel badges a lane from SlotInfo::mapped_tool while the filament
        // panel's op buttons resolve their lane through tool_to_slot_map;
        // writing only the forward entry here left the badge on the lane the
        // tool had just been moved away from.
        helix::printer::assign_tool_slot(system_info_, tool_number, slot_index);
    }

    // Send ASSIGN_TOOL: assign physical tool to respond to T<tool_number> commands
    std::ostringstream cmd;
    cmd << "ASSIGN_TOOL TOOL=" << physical_tool_name << " N=" << tool_number;

    spdlog::info("[AMS ToolChanger] Remapping T{} -> physical {} (slot {})", tool_number,
                 physical_tool_name, slot_index);
    return execute_gcode(cmd.str());
}

helix::printer::ToolMappingCapabilities
AmsBackendToolChanger::get_tool_mapping_capabilities() const {
    // klipper-toolchanger supports ASSIGN_TOOL for tool remapping
    return {true, true, "Tool reassignment via ASSIGN_TOOL"}; // i18n: do not translate
}

std::vector<int> AmsBackendToolChanger::get_tool_mapping() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_.tool_to_slot_map;
}

AmsError AmsBackendToolChanger::reset_tool_mappings() {
    // Restore identity mapping: each tool number maps to its own physical tool.
    // Sends ASSIGN_TOOL for each tool that was remapped away from identity.
    std::vector<std::pair<int, std::string>> remaps_needed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        int tool_count = static_cast<int>(tool_names_.size());
        for (int i = 0; i < tool_count; ++i) {
            if (i < static_cast<int>(system_info_.tool_to_slot_map.size()) &&
                system_info_.tool_to_slot_map[i] != i) {
                // Tool i is currently mapped to a different slot — restore to identity
                remaps_needed.emplace_back(i, tool_names_[i]);
                system_info_.tool_to_slot_map[i] = i;
            }
        }
        // Re-derive every slot's mapped_tool from the identity map just
        // written. The loop above touches only the forward direction, so
        // without this the lane badges kept showing the remap the user just
        // reset — the panel and the op buttons disagreeing again, one frame
        // after we told the user the reset succeeded.
        if (!remaps_needed.empty()) {
            helix::printer::sync_tool_map_from_forward(system_info_,
                                                       /*identity_fallback=*/true);
        }
    }

    if (remaps_needed.empty()) {
        spdlog::debug("[AMS ToolChanger] All tools already at identity mapping");
        return AmsErrorHelper::success();
    }

    AmsError last_error = AmsErrorHelper::success();
    for (const auto& [tool_num, tool_name] : remaps_needed) {
        std::ostringstream cmd;
        cmd << "ASSIGN_TOOL TOOL=" << tool_name << " N=" << tool_num;
        spdlog::info("[AMS ToolChanger] Resetting T{} -> physical {} (identity)", tool_num,
                     tool_name);
        auto err = execute_gcode(cmd.str());
        if (err.result != AmsResult::SUCCESS) {
            last_error = err;
        }
    }

    spdlog::info("[AMS ToolChanger] Reset {} tool mapping(s) to identity", remaps_needed.size());
    return last_error;
}

// ============================================================================
// Bypass Mode (Not Applicable)
// ============================================================================

AmsError AmsBackendToolChanger::enable_bypass() {
    return AmsErrorHelper::not_supported("Bypass mode");
}

AmsError AmsBackendToolChanger::disable_bypass() {
    return AmsErrorHelper::not_supported("Bypass mode");
}

bool AmsBackendToolChanger::is_bypass_active() const {
    return false; // Tool changers never have bypass
}

// ============================================================================
// Device Actions (stub - not applicable for tool changers)
// ============================================================================

std::vector<helix::printer::DeviceSection> AmsBackendToolChanger::get_device_sections() const {
    // Tool changers don't expose device-specific actions
    return {};
}

std::vector<helix::printer::DeviceAction> AmsBackendToolChanger::get_device_actions() const {
    // Tool changers don't expose device-specific actions
    return {};
}

AmsError AmsBackendToolChanger::execute_device_action(const std::string& action_id,
                                                      const std::any& value) {
    (void)action_id;
    (void)value;
    return AmsErrorHelper::not_supported("Device actions");
}
