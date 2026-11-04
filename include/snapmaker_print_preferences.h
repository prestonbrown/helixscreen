// include/snapmaker_print_preferences.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "json_fwd.h"

#include <optional>
#include <string>
#include <vector>

/**
 * @file snapmaker_print_preferences.h
 * @brief The U1's stored print preferences: how to read them, how to write them.
 *
 * `print_task_config` holds settings the firmware keeps across prints and
 * reboots, and the gcode the slicer emits is gated on them. `SET_PRINT_PREFERENCES`
 * is the setter, and it is a SETTER: a parameter it is not given keeps its
 * stored value. So a write must carry only the fields that changed, and a read
 * must distinguish "false" from "the frame did not mention it".
 *
 * Pure: no I/O, no LVGL, no Moonraker. The backend owns the plumbing.
 */
namespace helix::snapmaker {

/// Every field optional, because a delta frame is silent about what it omits.
/// `end_unload_filament` is empty rather than nullopt for the same reason.
struct PrintPreferences {
    std::optional<bool> auto_replenish;
    std::optional<bool> replenish_ignore_color;
    std::optional<bool> filament_entangle_detect;
    std::optional<bool> end_led_turn_off;
    std::optional<std::string> filament_entangle_sen; ///< "low" | "medium" | "high"
    std::vector<bool> end_unload_filament;            ///< one per toolhead
    /// One per toolhead: the head's filament is soft, so the end-of-print
    /// macro never unloads it. Read-only; no write carries it.
    std::vector<bool> filament_soft;
    /// Write-only: the write is for a print already running. The firmware
    /// refuses its print-scoped fields then (END_UNLOAD_FILAMENT among them)
    /// unless the command carries FORCE=1. Not a setting, so empty() ignores it.
    bool force = false;

    /// No setting reported. `filament_soft` describes the loaded filament,
    /// not a setting, so it alone does not count.
    [[nodiscard]] bool empty() const;

    /// Overlays what @p incoming sets onto this record. A delta frame that
    /// mentions one setting says nothing about the others, so a field @p
    /// incoming leaves unset keeps its value here.
    void merge(const PrintPreferences& incoming);
};

/// Pull whatever this status frame says about the stored preferences.
/// Fields the frame omits are left unset; nothing is inferred.
[[nodiscard]] PrintPreferences read_print_preferences(const nlohmann::json& status);

/// The one-line command that writes exactly the fields `changes` sets.
/// Empty string when `changes` sets none, so the caller can skip the send.
[[nodiscard]] std::string write_print_preferences_gcode(const PrintPreferences& changes);

} // namespace helix::snapmaker
