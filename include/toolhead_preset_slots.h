// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The home Toolhead widget's configurable preset buttons: up to three ordered slots, each a
// plate position, Park, a Z step, or a macro. Stored in the widget's per-instance config;
// the parse is non-throwing and falls back to the defaults.

#include "json_fwd.h"
#include "motion_presets.h"

#include <array>
#include <optional>
#include <string>

namespace helix {

inline constexpr std::size_t kToolheadPresetSlotCount = 3;
/// The Z step's distance, in millimetres.
inline constexpr double kToolheadZStepMm = 10.0;

struct ToolheadPresetSlot {
    enum class Kind { Empty, Position, Park, DockTool, ZUp, ZDown, Macro };
    Kind kind = Kind::Empty;
    MotionPreset position = MotionPreset::Center; ///< Kind::Position only
    std::string macro;                            ///< Kind::Macro only
};

using ToolheadPresetSlots = std::array<ToolheadPresetSlot, kToolheadPresetSlotCount>;

/// Front, Center, then Park, or Dock tool where @p can_dock_tool (the machine can put its
/// mounted tool back in its dock).
[[nodiscard]] ToolheadPresetSlots default_toolhead_preset_slots(bool can_dock_tool = false);

/// The widget config's "presets" array: [{"kind":"position","position":"front"},
/// {"kind":"park"}, {"kind":"tool_park"}, {"kind":"z_up"}, {"kind":"z_down"},
/// {"kind":"macro","macro":"NAME"},
/// {"kind":"empty"}], positions named by motion_preset_token(). A missing or malformed array yields
/// the defaults for @p can_dock_tool; a malformed entry yields Empty in its place. A saved
/// array is returned as saved, whatever @p can_dock_tool says.
[[nodiscard]] ToolheadPresetSlots toolhead_preset_slots_from_config(const nlohmann::json& config,
                                                                    bool can_dock_tool = false);
/// Whether @p config holds a saved "presets" array, so its slots are the user's and no default
/// applies.
[[nodiscard]] bool toolhead_preset_slots_saved(const nlohmann::json& config);
void toolhead_preset_slots_to_config(const ToolheadPresetSlots& slots, nlohmann::json& config);

/// Whether the connected machine can put its mounted tool back in its dock now
/// (usable_tool_park() on the AMS backend).
[[nodiscard]] bool toolhead_can_dock_tool();

/// Whether the slot needs every axis homed and the lift rule: Position, Park, ZUp, ZDown.
/// DockTool is not one: the tool park path applies its own refusals.
[[nodiscard]] bool toolhead_slot_moves_toolhead(const ToolheadPresetSlot& slot);

/// The slot's button text in the current language: the position or action, or the macro's
/// display name. Empty for Kind::Empty.
[[nodiscard]] std::string toolhead_preset_slot_label(const ToolheadPresetSlot& slot);
/// The slot's icon name (ui_icon_codepoints.h). Empty for Kind::Empty.
[[nodiscard]] const char* toolhead_preset_slot_icon(const ToolheadPresetSlot& slot);

} // namespace helix
