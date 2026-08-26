// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Per-tool z-offset.
//
// On a single-toolhead printer there is one z-offset and it is Klipper's live
// gcode_move offset, which helix::zoffset already owns. A tool changer adds a
// SECOND, independent offset per toolhead, and the two never interact: a
// baby-step does not disturb a tool's offset and selecting a tool does not
// disturb the baby-step. So the UI has to show and write them separately.
//
// The firmwares disagree on almost everything below that: where the value
// lives, what writes it, and whether persisting it restarts Klipper. This
// module is the ONLY place that knows. Generic code (ToolState, the tune panel)
// asks these functions and never names a firmware.
//
// Adding a firmware means adding one Provider to the table in
// tool_offsets.cpp - no call site changes.

#include <optional>
#include <string>
#include <vector>

#include "hv/json.hpp"

namespace helix {
class PrinterDiscovery;
}

namespace helix::tool_offsets {

/// Whether this printer keeps a z-offset per toolhead, separate from the live
/// gcode_move offset. False on every single-toolhead printer, which is what
/// gates the tune panel's tool selector.
bool supports_per_tool_z(const PrinterDiscovery& hw);

/// Extra Klipper status objects this printer needs subscribed for
/// read_tool_z_microns() to ever return a value. Empty when the offsets ride on
/// the `tool T*` objects the tool-changer subscription already requests.
std::vector<std::string> required_status_objects(const PrinterDiscovery& hw);

/// The z-offset of one tool, in microns, read out of a Moonraker status frame.
///
/// Takes the whole frame rather than one tool's payload because the firmwares
/// do not agree on where the value sits: klipper-toolchanger publishes it on
/// each `tool T<n>` object, while a MedusaHC-style machine keeps all four in a
/// single macro's variables.
///
/// nullopt means "no news", never "reset to zero" - Moonraker republishes only
/// the fields that CHANGED, so a frame carrying other tool state and not this
/// is routine.
///
/// @param status      full Moonraker status frame
/// @param tool_index  tool number, as used in T<n>
/// @param tool_name   Klipper's name for the tool ("T0"), for the firmwares
///                    that key their status object off it
std::optional<int> read_tool_z_microns(const nlohmann::json& status, int tool_index,
                                       const std::string& tool_name);

/// Gcode that sets tool @p tool_index's z-offset to @p microns, effective
/// immediately and NOT persisted. Empty when the printer has no per-tool
/// offset, or @p tool_index is negative.
///
/// Any embedded value is emitted as a bare decimal on purpose: this reaches a
/// firmware that parses it as a number (klipper-toolchanger runs VALUE= through
/// Python's ast.literal_eval()), so a display string like "+0.050mm" raises
/// rather than setting anything.
std::string set_tool_z_gcode(const PrinterDiscovery& hw, int tool_index, int microns);

/// Gcode that persists tool @p tool_index's z-offset as @p microns, so it
/// survives a restart. Empty when the printer has no per-tool offset, or
/// @p tool_index is negative. May be more than one line.
///
/// Self-sufficient: it writes the durable store AND whatever runtime value the
/// firmware actually prints with, so it is correct on its own and idempotent
/// after a set_tool_z_gcode() with the same value.
///
/// Whether this restarts Klipper depends on the firmware - ask
/// persist_requires_save_config().
std::string save_tool_z_gcode(const PrinterDiscovery& hw, int tool_index, int microns);

/// Whether save_tool_z_gcode() only stages the change and needs a SAVE_CONFIG
/// to commit it - which restarts Klipper, so the caller must drive it through
/// the same save-and-restart handling a probe calibration uses.
///
/// False means the save lands immediately and no restart follows. Getting this
/// backwards either leaves the value uncommitted or makes the UI wait out a
/// restart that never comes.
bool persist_requires_save_config(const PrinterDiscovery& hw);

/// Human-readable name of the matched offset model, for logging. Empty when
/// none.
std::string provider_name(const PrinterDiscovery& hw);

} // namespace helix::tool_offsets
