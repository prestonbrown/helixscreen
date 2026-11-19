// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file filament_mapping_plan.h
 * @brief Which slicer tools a file maps onto which lanes, before any card draws it
 *
 * The filament mapping card shows these answers and print start gates on them;
 * both ask here, so the two cannot reach different results for one file.
 */

#include "filament_mapper.h"

#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace helix::filament_mapping {

/// The visibility rule for a single extruder with no filament system: true
/// when a used tool (every tool, for an empty or nullopt set) names a
/// material. Reads AmsState and ToolState.
[[nodiscard]] bool shows_loaded_for(const std::vector<std::string>& materials,
                                    const std::optional<std::set<int>>& used_tools);

/// The lane-mapping visibility rule, without the card's widgets:
/// AMS availability, the bypass single-lane rule and the lane count. A
/// start with no file view asks it to know whether the view's card would
/// hold a mapping for the same file.
/// @param palette_colour_count Slicer colors in the palette
/// @param palette_tool_count   max(colors, materials), build_tool_info()'s count
/// @param used_tools           The scan's used tools; empty or nullopt reads as unknown
[[nodiscard]] bool shows_mapping_for(size_t palette_colour_count, size_t palette_tool_count,
                                     const std::optional<std::set<int>>& used_tools);

/// What the card holds for a file whose mapping the user has not edited.
struct Mapping {
    size_t palette_colour_count = 0; ///< colors.size()
    size_t palette_tool_count = 0;   ///< max(colors, materials), build_tool_info()'s count
    bool shows = false;              ///< shows_mapping_for() those counts and the used set
    std::vector<helix::GcodeToolInfo> tool_info; ///< narrowed to the used tools
    std::vector<helix::AvailableSlot> slots;     ///< empty unless shows
    std::vector<helix::ToolMapping> mappings;    ///< seeded, then narrowed; empty unless shows
};

/// The card's rule for a file, in its order: the tools from the palette,
/// the visibility rule, the slots and the seeded mappings when it shows,
/// then the used-tools filter. update() assigns from it, and a start with
/// no file view (print_start_inputs.cpp) asks it, so the two cannot reach
/// different gates for one file. Reads AmsState; no card state.
[[nodiscard]] Mapping compute_mapping(const std::vector<std::string>& colors,
                                      const std::vector<std::string>& materials,
                                      const std::optional<std::set<int>>& used_tools);

/// Build GcodeToolInfo list from color/material strings.
///
/// Pure/stateless: derives one GcodeToolInfo per tool (tool_index = i,
/// color_rgb parsed from colors[i], material = materials[i]) using only its
/// arguments. Callers can source per-tool info directly
/// from the same color/material data that feeds the card (Moonraker
/// metadata, populated on all platforms) without coupling to a card
/// INSTANCE whose tool_info_ is only populated on some code paths.
std::vector<helix::GcodeToolInfo> build_tool_info(const std::vector<std::string>& colors,
                                                  const std::vector<std::string>& materials);

/// build_tool_info(), narrowed to the tools a file actually prints with.
///
/// Pure/stateless. Keeps each kept entry's REAL gcode tool number in
/// `.tool_index` (not its position in the result), because the colour and
/// pre-flight paths downstream index by tool number: a print that uses only
/// T0 and T2 must not have T2's colour land at index 1. A tool with no
/// palette entry is dropped — the slicer palette bounds what can be known.
///
/// Shared by PrintSelectDetailView::get_used_tool_info() and
/// PrintStatusPanel::build_print_tool_info(), which are the file-browser and
/// live-print halves of the same question and were literal copies of this
/// loop. They must agree: the two previews colour the same print.
///
/// @param colors    Per-tool slicer palette ("#RRGGBB"), palette-ordinal indexed.
/// @param materials Per-tool material names, same indexing.
/// @param used      Tool numbers the file prints with (empty ⇒ empty result).
std::vector<helix::GcodeToolInfo> build_used_tool_info(const std::vector<std::string>& colors,
                                                       const std::vector<std::string>& materials,
                                                       const std::set<int>& used);

/// Compact parallel tool_info / mappings vectors to only the used tools.
///
/// Pure/stateless: filters BOTH vectors in lockstep, keeping only entries
/// whose `.tool_index` is in `used`, preserving order and `.tool_index`.
/// `nullopt` or an empty set ⇒ no-op (show all) — the safety rule that
/// prevents blanking the card pre-parse or on the headless single-extruder
/// path.
void apply_used_tools_filter(std::vector<helix::GcodeToolInfo>& tool_info,
                             std::vector<helix::ToolMapping>& mappings,
                             const std::optional<std::set<int>>& used);

/// Find a tool by its real gcode `.tool_index`, not by vector position.
///
/// `tool_info` may be used-filtered (compacted), so position no longer
/// equals `.tool_index`. Callers that have a `tool_index` (e.g. from a
/// ToolMapping or an unresolved-tools list) MUST look up through here rather
/// than `tool_info[tool_index]`. Returns nullptr if no entry matches.
const helix::GcodeToolInfo* find_by_tool_index(const std::vector<helix::GcodeToolInfo>& tool_info,
                                               int tool_index);

} // namespace helix::filament_mapping
