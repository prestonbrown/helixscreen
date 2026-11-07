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
#include <string>
#include <vector>

namespace helix {

class PrinterDiscovery;
struct ToolMapping;

/// printer.print.start parameters beyond `filename`, name -> value.
using PrintStartParams = std::map<std::string, std::string>;

namespace print_start {

/// Whether this printer's print start takes a per-print tool -> slot map.
/// Z-Mod's Moonraker gained `tools` together with its built-in AFC emulation,
/// and publishes no version or field that says so on its own, so the built-in
/// AFC (PrinterDiscovery::has_zmod_afc()) is the signal. A Z-Mod build with
/// `AFC=0`, or one older than the emulation, keeps routing by its own prompt
/// and tables, and is sent no map.
[[nodiscard]] bool takes_tool_slots(const PrinterDiscovery& hw);

/// The parameters carrying @p slot_numbers (G-code tool -> 1-based physical
/// slot) for a printer where takes_tool_slots() holds. Index is the tool, value
/// the slot, a tool with no slot left empty, so {T0: 2, T2: 1} is "2,,1".
/// Empty when no tool has a valid slot.
[[nodiscard]] PrintStartParams tool_slot_params(const std::map<int, int>& slot_numbers);

/// The parameters naming plate @p plate of the .3mf a print start sends, for a
/// printer that starts a .3mf's plates itself (print_3mf::PrinterReads::Plates).
/// Z-Mod's Moonraker takes it as `plateindex`. Empty for a plate below 1.
[[nodiscard]] PrintStartParams plate_params(int plate);

/// The backend a print's map belongs to: the first non-Auto row's (-1 on a
/// row is the primary, 0), else 0.
[[nodiscard]] int mapped_backend(const std::vector<ToolMapping>& mappings);

/// A print's full tool map on @p backend_idx, G-code tool -> 0-based slot:
/// every row the card holds for that backend, picked or seeded, since a map
/// taken at the start decides the whole print. An Auto row, and a tool with no
/// row, stay out: the firmware's own default answers for them, never a lane
/// guessed here.
[[nodiscard]] std::map<int, int> full_tool_slots(const std::vector<ToolMapping>& mappings,
                                                 int backend_idx);

} // namespace print_start
} // namespace helix
