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

#include "json_fwd.h"
#include "print_3mf.h"

#include <map>
#include <string>
#include <vector>

namespace helix {

class PrinterDiscovery;
struct ToolMapping;
enum class PluginFeatureState;

/// printer.print.start parameters beyond `filename`, name -> value.
using PrintStartParams = std::map<std::string, std::string>;

namespace print_start {

/// Whether this printer's print start takes a per-print tool -> slot map.
/// Z-Mod's Moonraker says so in server.info
/// (PrinterDiscovery::server_takes_print_start_tools(), from
/// server_takes_tool_slots()), whatever its AFC setting. A Z-Mod build from
/// before that flag reads `tools` only alongside its built-in AFC emulation
/// (PrinterDiscovery::has_zmod_afc()), so the emulation still counts. Anything
/// else keeps routing by its own prompt and tables, and is sent no map.
[[nodiscard]] bool takes_tool_slots(const PrinterDiscovery& hw);

/// Whether a server.info `result` advertises that printer.print.start takes
/// `tools`: Z-Mod's Moonraker lists `print_start_tools` in `zmod.features`.
/// False for any other shape, a missing key included.
[[nodiscard]] bool server_takes_tool_slots(const nlohmann::json& server_info_result);

/// The parameters carrying @p slot_numbers (G-code tool -> 1-based physical
/// slot) for a printer where takes_tool_slots() holds. Index is the tool, value
/// the slot, a tool with no slot left empty, so {T0: 2, T2: 1} is "2,,1".
/// Empty when no tool has a valid slot.
[[nodiscard]] PrintStartParams tool_slot_params(const std::map<int, int>& slot_numbers);

/// What a print start along @p route sends beside its filename: @p tool_params
/// (the print's tool map, tool_slot_params()) and, for a .3mf plate the printer
/// starts itself (print_3mf::RouteKind::NativePlate), that plate. Z-Mod's
/// Moonraker takes the plate as `plateindex`. Every print start builds its
/// parameters here; nothing else adds a plate or a tool map.
[[nodiscard]] PrintStartParams start_request_params(const print_3mf::Route& route,
                                                    PrintStartParams tool_params);

/// The HelixPrint plugin feature saying the starts it makes (print_modified,
/// print_edited) pass the request's `tools` on to Moonraker.
inline constexpr const char* kPluginStartToolsFeature = "print_start_tools";
inline constexpr int kPluginStartToolsMinVersion = 1;

/// Whether a start the HelixPrint plugin makes loses @p params' tool map: they
/// carry one and @p plugin_start_tools, the state of kPluginStartToolsFeature,
/// is not Ready (a plugin older than the feature, or a Moonraker whose start
/// takes no map). The printer then routes that print by its own prompt.
[[nodiscard]] bool plugin_drops_tool_map(const PrintStartParams& params,
                                         PluginFeatureState plugin_start_tools);

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
