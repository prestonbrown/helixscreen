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
    enum class Kind { Empty, Position, Park, ZUp, ZDown, Macro };
    Kind kind = Kind::Empty;
    MotionPreset position = MotionPreset::Center; ///< Kind::Position only
    std::string macro;                            ///< Kind::Macro only
};

using ToolheadPresetSlots = std::array<ToolheadPresetSlot, kToolheadPresetSlotCount>;

/// Front, Center, Park.
[[nodiscard]] ToolheadPresetSlots default_toolhead_preset_slots();

/// The widget config's "presets" array: [{"kind":"position","position":"front"},
/// {"kind":"park"}, {"kind":"z_up"}, {"kind":"z_down"}, {"kind":"macro","macro":"NAME"},
/// {"kind":"empty"}], positions named by motion_preset_token(). A missing or malformed array yields
/// the defaults; a malformed entry yields Empty in its place.
[[nodiscard]] ToolheadPresetSlots toolhead_preset_slots_from_config(const nlohmann::json& config);
void toolhead_preset_slots_to_config(const ToolheadPresetSlots& slots, nlohmann::json& config);

/// Whether the slot needs every axis homed and the lift rule: Position, Park, ZUp, ZDown.
[[nodiscard]] bool toolhead_slot_moves_toolhead(const ToolheadPresetSlot& slot);

/// The slot's button text in the current language: the position or action, or the macro's
/// display name. Empty for Kind::Empty.
[[nodiscard]] std::string toolhead_preset_slot_label(const ToolheadPresetSlot& slot);
/// The slot's icon name (ui_icon_codepoints.h). Empty for Kind::Empty.
[[nodiscard]] const char* toolhead_preset_slot_icon(const ToolheadPresetSlot& slot);

} // namespace helix
