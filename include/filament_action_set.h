// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file filament_action_set.h
 * @brief Which filament actions a surface offers, decided once for every backend.
 *
 * Load and Unload mean filament on every printer. A surface describes the
 * machine as a FilamentHeadView, plain values read off the backend's
 * capability answers, and maps the FilamentActionSet that comes back onto its
 * own widgets. Nothing here knows which backend it is looking at.
 *
 * Mount and Park are the only actions that read which head is selected or
 * mounted. Load and Unload never do: the selected or mounted head is only the
 * picker's preselection.
 */

#pragma once

#include "ams_backend.h"

#include <optional>
#include <string>
#include <vector>

namespace helix::ui {

/// One head, as the decision needs it.
struct HeadState {
    HeadFilamentOps ops;
    std::optional<int> source_slot;     ///< AmsBackend::head_source_slot(): what a Load feeds
    std::optional<int> unload_slot;     ///< The slot an Unload of this head names
    std::optional<bool> source_present; ///< slot_presence() of the source; nullopt = cannot tell
    std::optional<bool> at_nozzle;      ///< The source is fed to the nozzle already
    std::optional<bool> has_filament;   ///< AmsBackend::head_has_filament(); nullopt = cannot tell
};

struct FilamentHeadView {
    bool backend_present = false;
    int max_heads_per_op = 1;
    std::vector<HeadState> heads;
    bool heads_dock = false;    ///< AmsBackend::carriage_can_be_empty(): heads mount and park
    bool supports_park = false; ///< AmsBackend::supports_tool_park()
    int selected_head = -1;     ///< The picker's preselection and Mount's target; -1 for none
    int mounted_head = -1;      ///< The head on the carriage; -1 for none or not a docking machine
    bool system_busy = false;   ///< AmsSystemInfo::is_busy()
    bool print_blocks_op = false; ///< print_blocks_filament_op()
    /// print_blocks_filament_load(); unset means Load answers to print_blocks_op.
    std::optional<bool> print_blocks_load{};
};

struct ActionState {
    bool visible = false;
    bool enabled = false;
    std::string reason; ///< Translated; why a visible action is disabled, or empty
};

struct FilamentActionSet {
    ActionState load;
    ActionState unload;
    ActionState mount;
    ActionState park;
    bool unload_opens_picker = false; ///< Several heads: Unload asks which
};

/// What the head picker is choosing heads for. SeatByHand claims a tool the
/// user holds on the head: any tool, none suggested.
enum class HeadPickOp { Load, Unload, Mount, SeatByHand };

/// Whether @p op acts on exactly one head, naming no slot.
[[nodiscard]] constexpr bool single_select(HeadPickOp op) {
    return op == HeadPickOp::Mount || op == HeadPickOp::SeatByHand;
}

struct HeadPickRow {
    int head = -1;
    int slot = -1; ///< What the op names: the source for Load, the unload slot for Unload, else -1
    bool enabled = false;
    std::string reason; ///< Translated; why a disabled row is disabled
    bool preselected = false;
};

/// Can @p head take a load right now: automatic, with an assigned source that
/// is not known empty and not already at the nozzle.
[[nodiscard]] bool head_can_load(const HeadState& head);

/// Can @p head take an unload right now: automatic, with a slot to name, and
/// not known to be empty.
[[nodiscard]] bool head_can_unload(const HeadState& head);

/// The sidebar's actions: Load and Unload, or on a docking machine with no
/// automatic filament op at all, Mount and Park in their place.
[[nodiscard]] FilamentActionSet decide_filament_actions(const FilamentHeadView& view);

/// The actions for one slot's head (the slot context menu). @p head is
/// AmsBackend::head_for_slot(slot), -1 for a slot that feeds no head.
[[nodiscard]] FilamentActionSet decide_slot_actions(const FilamentHeadView& view, int head);

/// The head picker's rows for @p op, one per head.
[[nodiscard]] std::vector<HeadPickRow> decide_head_picker_rows(const FilamentHeadView& view,
                                                               HeadPickOp op);

} // namespace helix::ui
