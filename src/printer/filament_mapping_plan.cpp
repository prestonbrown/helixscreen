// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_mapping_plan.h"

#include "ams_state.h"
#include "color_utils.h"
#include "print_start_checks.h"
#include "tool_state.h"

#include <algorithm>

namespace helix::filament_mapping {

bool shows_loaded_for(const std::vector<std::string>& materials,
                      const std::optional<std::set<int>>& used_tools) {
    // One extruder fed by hand: a printer with a filament system or several toolheads
    // has lanes or toolheads to map, which shows_mapping_for() decides.
    if (AmsState::instance().is_available() ||
        helix::ToolState::instance().has_multiple_toolheads()) {
        return false;
    }
    for (size_t i = 0; i < materials.size(); ++i) {
        const bool used =
            !used_tools || used_tools->empty() || used_tools->count(static_cast<int>(i)) > 0;
        if (used && !materials[i].empty()) {
            return true;
        }
    }
    return false;
}

bool shows_mapping_for(size_t palette_colour_count, size_t palette_tool_count,
                       const std::optional<std::set<int>>& used_tools) {
    auto& ams = AmsState::instance();
    if (!ams.is_available()) {
        return false;
    }

    // NO editable-backend gate here. It once existed to avoid a dead control on
    // Snapmaker U1 / ACE, back when a second surface (the print-detail FILAMENTS
    // card) drew the same chips there. That surface is gone, so hiding here would
    // show the user nothing at all. Whether a TAP does anything is a separate
    // question, answered by PrintSelectDetailView::current_remap_block().

    // A dead-control rule survives here even without the editable-backend gate
    // above: with bypass engaged a single-tool print takes its filament from
    // the external spool, and print_start_checks.cpp compares
    // against that spool instead of the lanes (the `any_bypass_active &&
    // print_lane_requirement(...) <= 1` short-circuit). Offering a lane mapping
    // there claims something the print will not do — a K2 Plus user read the
    // chips as "this maps to lane 2", tapped one to confirm it, and started a
    // print that ran on the bypass spool. A genuinely multi-lane print still
    // uses the mapping with bypass on, so this only hides the <= 1 case.
    //
    // print_lane_requirement() is shared with the gate rather than reimplemented
    // here: it prefers the scan's tools_used and falls back to the palette, and
    // a second copy of that precedence would drift into exactly the mismatch
    // this hides.
    //
    // The fallback is the COLOUR count, not the tool count: the two differ when
    // a slicer reports materials and no colours (every OrcaSlicer file on a
    // K2 Plus), and this gate has always asked "how many lanes does the file
    // want", which is a question about colours.
    if (ams.any_bypass_active() &&
        helix::print_lane_requirement(used_tools ? *used_tools : std::set<int>{},
                                      palette_colour_count) <= 1) {
        return false;
    }

    // Nothing to map. Equivalent to the build_tool_info() result being empty:
    // it emits max(colours, materials) entries and filters none.
    if (palette_tool_count == 0) {
        return false;
    }

    // Moved from PrintSelectDetailView::swatches_card_visible_for(): on a
    // multi-tool printer any referenced tool is worth showing (lane identity
    // matters); on a single extruder it takes 2+ tools to be a manual-swap
    // multi-colour file rather than an ordinary single-colour print.
    //
    // print_lane_requirement() (shared with the bypass gate above) is reused
    // rather than a hand-written used_tools_->size() : tool_info_.size()
    // fallback — every OTHER reader of used_tools_ in this class treats an
    // empty set as "no answer, show all" (the bypass gate above,
    // apply_used_tools_filter()'s documented contract), and a second,
    // divergent copy of "empty means fall back to the palette count" is
    // exactly the drift this merge exists to remove. An empty set is not a
    // not-yet-computed sentinel either — PrintSelectDetailView persists a
    // successful zero-tool scan as a legitimate single-extruder answer.
    const int ams_slots = lv_subject_get_int(AmsState::instance().get_slot_count_subject());
    const bool is_multi_tool_printer =
        helix::ToolState::instance().is_multi_tool() || ams_slots > 1;
    const size_t tool_count = helix::print_lane_requirement(
        used_tools ? *used_tools : std::set<int>{}, palette_tool_count);
    if (!(is_multi_tool_printer ? tool_count > 0 : tool_count > 1)) {
        return false;
    }

    return true;
}

Mapping compute_mapping(const std::vector<std::string>& colors,
                        const std::vector<std::string>& materials,
                        const std::optional<std::set<int>>& used_tools) {
    Mapping m;
    m.palette_colour_count = colors.size();
    m.palette_tool_count = std::max(colors.size(), materials.size());
    // A pure function of the palette, built whether or not the card shows:
    // the print-start gate reads the tools either way.
    m.tool_info = build_tool_info(colors, materials);
    m.shows = shows_mapping_for(m.palette_colour_count, m.palette_tool_count, used_tools);
    if (m.shows) {
        auto& ams = AmsState::instance();
        // Seeded through the shared rule, which reads the EFFECTIVE auto-match
        // predicate: AmsState::effective_auto_match() overrides the raw setting
        // on backends (Snapmaker U1) where the two differ.
        m.slots = ams.collect_available_slots();
        m.mappings = ams.seed_tool_mappings(m.tool_info, m.slots);
    }
    // Seeded from the whole palette, then narrowed to the tools the file uses;
    // nullopt or empty keeps them all.
    apply_used_tools_filter(m.tool_info, m.mappings, used_tools);
    return m;
}

std::vector<helix::GcodeToolInfo> build_used_tool_info(const std::vector<std::string>& colors,
                                                       const std::vector<std::string>& materials,
                                                       const std::set<int>& used) {
    if (used.empty()) {
        return {};
    }
    const auto all_tool_info = build_tool_info(colors, materials);

    std::vector<helix::GcodeToolInfo> tools;
    tools.reserve(used.size());
    for (int tool : used) {
        if (tool >= 0 && static_cast<size_t>(tool) < all_tool_info.size()) {
            auto info = all_tool_info[static_cast<size_t>(tool)];
            info.tool_index = tool; // real gcode tool number, not palette ordinal
            tools.push_back(info);
        }
    }
    return tools;
}

std::vector<helix::GcodeToolInfo> build_tool_info(const std::vector<std::string>& colors,
                                                  const std::vector<std::string>& materials) {
    std::vector<helix::GcodeToolInfo> tools;

    // Use the larger of colors or materials to determine tool count.
    // If both are empty, return empty — the card will be hidden.
    size_t count = std::max(colors.size(), materials.size());
    if (count == 0) {
        return tools;
    }

    for (size_t i = 0; i < count; ++i) {
        helix::GcodeToolInfo tool;
        tool.tool_index = static_cast<int>(i);

        // Parse color. A missing or unparsable entry leaves color_rgb as a
        // NEUTRAL STAND-IN, not an answer: Moonraker omits filament_colors
        // entirely for some slicers (every OrcaSlicer file on a K2 Plus reports
        // filament_type and nothing else), and the palette is backfilled later
        // from the G-code footer or the viewer. Flagging it keeps the stand-in
        // from being read as the file's choice — see GcodeToolInfo::color_known.
        if (i < colors.size() && !colors[i].empty()) {
            auto parsed = helix::parse_hex_color(colors[i]);
            tool.color_rgb = parsed.value_or(0x808080);
            tool.color_known = parsed.has_value();
        } else {
            tool.color_rgb = 0x808080;
            tool.color_known = false;
        }

        // Material
        if (i < materials.size()) {
            tool.material = materials[i];
        }

        tools.push_back(std::move(tool));
    }

    return tools;
}

void apply_used_tools_filter(std::vector<helix::GcodeToolInfo>& tool_info,
                             std::vector<helix::ToolMapping>& mappings,
                             const std::optional<std::set<int>>& used) {
    // nullopt OR empty set ⇒ no filter (show all). Safety rule: never blank the
    // card pre-parse, and never hide everything on the headless single-extruder
    // path (where the used set is empty forever).
    if (!used || used->empty()) {
        return;
    }
    const std::set<int>& keep = *used;

    // Filter BOTH vectors independently by their own .tool_index — they are
    // built parallel (mappings_[i].tool_index == tool_info_[i].tool_index), so
    // the same predicate compacts them in lockstep. std::remove_if preserves
    // order; .tool_index is retained (used for the "T%d" label + modal rows).
    tool_info.erase(std::remove_if(tool_info.begin(), tool_info.end(),
                                   [&keep](const helix::GcodeToolInfo& t) {
                                       return keep.count(t.tool_index) == 0;
                                   }),
                    tool_info.end());
    mappings.erase(std::remove_if(mappings.begin(), mappings.end(),
                                  [&keep](const helix::ToolMapping& m) {
                                      return keep.count(m.tool_index) == 0;
                                  }),
                   mappings.end());
}

const helix::GcodeToolInfo* find_by_tool_index(const std::vector<helix::GcodeToolInfo>& tool_info,
                                               int tool_index) {
    for (const auto& t : tool_info) {
        if (t.tool_index == tool_index) {
            return &t;
        }
    }
    return nullptr;
}

} // namespace helix::filament_mapping
