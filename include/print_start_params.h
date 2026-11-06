// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Parameters a print start carries beside the filename, and the firmwares that
// take the print's tool -> slot map that way.
//
// Most firmwares route a print's tools from a table they keep (SET_MAP,
// SET_EXTRUDER_SLOT, ...). Z-Mod's Moonraker instead takes the map for one print
// as a `tools` parameter of printer.print.start and hands it to its start macro,
// and without it opens its own colour prompt. This module is the only place
// that knows which firmwares do that and how the parameter is spelled; filament
// backends ask it, and the print-start path forwards whatever they return.

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace helix {

class PrinterDiscovery;

/// printer.print.start parameters beyond `filename`, name -> value.
using PrintStartParams = std::map<std::string, std::string>;

namespace print_start {

/// Whether this printer's print start takes a per-print tool -> slot map.
/// Z-Mod is recognised by its `zmod_color` object, which both the AD5X and the
/// Creator 5 Pro publish whatever their AFC setting.
[[nodiscard]] bool takes_tool_slots(const PrinterDiscovery& hw);

/// The physical slot a lane name carries in its trailing digits: Z-Mod's lane
/// `laneN` is slot N, IFS Jacker lanes past 4 included. nullopt when the name
/// ends in no digits or names slot 0.
[[nodiscard]] std::optional<int> slot_number_of_lane(std::string_view lane);

/// The parameters carrying @p slot_numbers (G-code tool -> 1-based physical
/// slot) for a printer where takes_tool_slots() holds. Index is the tool, value
/// the slot, a tool with no slot left empty, so {T0: 2, T2: 1} is "2,,1".
/// Empty when no tool has a valid slot.
[[nodiscard]] PrintStartParams tool_slot_params(const std::map<int, int>& slot_numbers);

} // namespace print_start
} // namespace helix
