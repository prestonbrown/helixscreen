// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_mapper.h"

#include "filament_database.h"
#include "filament_variants.h"
#include "lvgl/src/others/translation/lv_translation.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace helix {

std::string FilamentMapper::format_slot_label(const AvailableSlot& slot) {
    char buf[192];

    // Build material suffix
    const char* material_str = nullptr;
    if (slot.is_empty) {
        material_str = lv_tr("Empty");
    } else if (!slot.material.empty()) {
        material_str = slot.material.c_str();
    }

    if (slot.unit_display_name.empty()) {
        // Single-unit: "Slot 2" or "Slot 2: PLA"
        if (material_str) {
            snprintf(buf, sizeof(buf), "%s %d: %s", lv_tr("Slot"), slot.local_slot_index + 1,
                     material_str);
        } else {
            snprintf(buf, sizeof(buf), "%s %d", lv_tr("Slot"), slot.local_slot_index + 1);
        }
    } else {
        // Multi-unit: "Turtle 1 · Slot 2" or "Turtle 1 · Slot 2: PLA"
        // unit_display_name is not translated — it's a user-configured AFC name
        if (material_str) {
            snprintf(buf, sizeof(buf), "%s \xc2\xb7 %s %d: %s", slot.unit_display_name.c_str(),
                     lv_tr("Slot"), slot.local_slot_index + 1, material_str);
        } else {
            snprintf(buf, sizeof(buf), "%s \xc2\xb7 %s %d", slot.unit_display_name.c_str(),
                     lv_tr("Slot"), slot.local_slot_index + 1);
        }
    }
    return buf;
}

int FilamentMapper::color_distance(uint32_t a, uint32_t b) {
    int r1 = (a >> 16) & 0xFF;
    int g1 = (a >> 8) & 0xFF;
    int b1 = a & 0xFF;

    int r2 = (b >> 16) & 0xFF;
    int g2 = (b >> 8) & 0xFF;
    int b2 = b & 0xFF;

    // Weighted RGB distance using standard luminance coefficients.
    // Weights: R=0.30, G=0.59, B=0.11 (standard luminance)
    int dr = r1 - r2;
    int dg = g1 - g2;
    int db = b1 - b2;

    int dist_sq = (dr * dr * 30 + dg * dg * 59 + db * db * 11) / 100;
    return static_cast<int>(std::sqrt(static_cast<double>(dist_sq)));
}

bool FilamentMapper::colors_match(uint32_t color_a, uint32_t color_b) {
    return color_distance(color_a, color_b) <= COLOR_MATCH_TOLERANCE;
}

bool FilamentMapper::materials_match(const std::string& a, const std::string& b) {
    // One implementation of "same polymer?", shared with endless-spool backup
    // eligibility. It lives in filament_variants beside the family reduction it
    // depends on rather than here, so a backend can ask the question without
    // depending on the mapper.
    return filament::materials_compatible(a, b);
}

SlotKey FilamentMapper::find_closest_color_slot(uint32_t target_color,
                                                const std::string& target_material,
                                                const std::vector<AvailableSlot>& slots) {
    SlotKey best_key{-1, -1};
    int best_distance = COLOR_MATCH_TOLERANCE + 1; // Must be within tolerance

    for (const auto& slot : slots) {
        // Empty slots have no filament — their reported color is stale (left over
        // from the last spool), so they must never attract a color match. Matching
        // a tool to an empty lane paints the render in a color that can't print and
        // routes the print to a lane the firmware will reject (the v0.91 "wrong
        // filament" report: a white tool matched the empty white lane instead of
        // being substituted to a loaded lane).
        if (slot.is_empty) {
            continue;
        }
        // Skip slots with incompatible materials (unless either side has no info)
        if (!target_material.empty() && !slot.material.empty() &&
            !materials_match(target_material, slot.material)) {
            continue;
        }

        int dist = color_distance(target_color, slot.color_rgb);
        if (dist < best_distance) {
            best_distance = dist;
            best_key = slot.key();
        }
    }

    return best_key;
}

std::vector<ToolMapping> FilamentMapper::compute_defaults(const std::vector<GcodeToolInfo>& tools,
                                                          const std::vector<AvailableSlot>& slots) {
    std::vector<ToolMapping> mappings;
    mappings.reserve(tools.size());

    // Track which slots have been claimed for positional fallback deduplication.
    // Color matching allows slot re-use, but positional fallback avoids it.
    std::vector<SlotKey> used_slots;

    // A fallback must never auto-assign a lane whose material is KNOWN to be
    // incompatible with the tool (e.g. a PLA tool into a PETG or TPU lane) — that
    // misroutes the print on every backend. Unknown/empty materials can't be
    // proven incompatible, so they stay eligible (backends that don't publish
    // material keep the positional fallback). Explicit firmware mappings
    // (Priority 1) are still honored with a mismatch warning; only the guesses
    // here are gated.
    auto material_blocked = [](const GcodeToolInfo& tool, const AvailableSlot& slot) {
        return !tool.material.empty() && !slot.material.empty() &&
               !materials_match(tool.material, slot.material);
    };

    for (const auto& tool : tools) {
        ToolMapping mapping;
        mapping.tool_index = tool.tool_index;

        // Priority 1: Check firmware mapping (slot already assigned to this tool)
        bool firmware_matched = false;
        for (const auto& slot : slots) {
            if (slot.is_empty) {
                continue;
            }
            if (slot.current_tool_mapping == tool.tool_index) {
                mapping.mapped_slot = slot.slot_index;
                mapping.mapped_backend = slot.backend_index;
                mapping.reason = ToolMapping::MatchReason::FIRMWARE_MAPPING;

                if (!tool.material.empty() && !slot.material.empty() &&
                    !materials_match(tool.material, slot.material)) {
                    mapping.material_mismatch = true;
                }

                used_slots.push_back(slot.key());
                firmware_matched = true;
                break;
            }
        }

        if (firmware_matched) {
            mappings.push_back(mapping);
            continue;
        }

        // Priority 2: Color match
        auto [slot_idx, backend_idx] =
            find_closest_color_slot(tool.color_rgb, tool.material, slots);
        if (slot_idx >= 0) {
            mapping.mapped_slot = slot_idx;
            mapping.mapped_backend = backend_idx;
            mapping.reason = ToolMapping::MatchReason::COLOR_MATCH;

            // Find the slot to check material compatibility
            for (const auto& slot : slots) {
                if (slot.slot_index == slot_idx && slot.backend_index == backend_idx) {
                    if (!tool.material.empty() && !slot.material.empty() &&
                        !materials_match(tool.material, slot.material)) {
                        mapping.material_mismatch = true;
                    }
                    break;
                }
            }

            used_slots.push_back({slot_idx, backend_idx});
            mappings.push_back(mapping);
            continue;
        }

        // Priority 3: Positional fallback — assign to the slot matching the tool index
        {
            int tool_idx = tool.tool_index;
            for (const auto& slot : slots) {
                // Only a LOADED lane can serve as a substitute — an empty lane has
                // no filament to route the tool to.
                if (slot.slot_index == tool_idx && slot.backend_index == 0 && !slot.is_empty) {
                    auto key = slot.key();
                    if (std::find(used_slots.begin(), used_slots.end(), key) == used_slots.end()) {
                        mapping.mapped_slot = slot.slot_index;
                        mapping.mapped_backend = slot.backend_index;
                        mapping.reason = ToolMapping::MatchReason::COLOR_MATCH;
                        // The tool's own positional lane is a deliberate default:
                        // assign it but flag a material mismatch so PrintStartController
                        // can warn. (Only the material-blind "any unclaimed lane"
                        // fallback below refuses incompatible lanes.)
                        if (!tool.material.empty() && !slot.material.empty() &&
                            !materials_match(tool.material, slot.material)) {
                            mapping.material_mismatch = true;
                        }
                        used_slots.push_back(key);
                    }
                    break;
                }
            }
            // No positional lane: rather than grab an arbitrary unclaimed lane (the
            // old material-blind behavior that misrouted prints — a PLA tool into a
            // PETG/TPU lane), take the first unclaimed lane that is not known to be
            // incompatible. If none qualifies the tool stays unmatched for preflight.
            if (mapping.mapped_slot < 0) {
                for (const auto& slot : slots) {
                    auto key = slot.key();
                    if (!slot.is_empty &&
                        std::find(used_slots.begin(), used_slots.end(), key) == used_slots.end() &&
                        !material_blocked(tool, slot)) {
                        mapping.mapped_slot = slot.slot_index;
                        mapping.mapped_backend = slot.backend_index;
                        mapping.reason = ToolMapping::MatchReason::COLOR_MATCH;
                        used_slots.push_back(key);
                        break;
                    }
                }
            }
        }

        // If no priority matched, mark as AUTO (let firmware decide)
        if (mapping.mapped_slot < 0) {
            mapping.is_auto = true;
            mapping.reason = ToolMapping::MatchReason::AUTO;
        }

        mappings.push_back(mapping);
    }

    return mappings;
}

std::vector<uint32_t>
FilamentMapper::resolve_display_colors(const std::vector<GcodeToolInfo>& tools,
                                       const std::vector<ToolMapping>& mappings,
                                       const std::vector<AvailableSlot>& slots) {
    std::vector<uint32_t> colors;
    colors.reserve(mappings.size());

    for (size_t i = 0; i < mappings.size(); ++i) {
        const auto& mapping = mappings[i];
        uint32_t color = (i < tools.size()) ? tools[i].color_rgb : 0x808080;

        if (!mapping.is_auto && mapping.mapped_slot >= 0) {
            for (const auto& s : slots) {
                if (s.slot_index == mapping.mapped_slot &&
                    s.backend_index == mapping.mapped_backend) {
                    color = s.color_rgb;
                    break;
                }
            }
        }
        colors.push_back(color);
    }
    return colors;
}

std::vector<ToolMapping> FilamentMapper::effective_mappings(const std::vector<GcodeToolInfo>& tools,
                                                            const std::vector<AvailableSlot>& slots,
                                                            bool auto_color_map) {
    if (auto_color_map) {
        // Color/type matching: clear firmware mappings so they don't pre-empt
        // color matches (mirrors FilamentMappingCard's auto-match seeding).
        auto slots_for_matching = slots;
        for (auto& s : slots_for_matching) {
            s.current_tool_mapping = -1;
        }
        return compute_defaults(tools, slots_for_matching);
    }
    return use_current_assignments(tools, slots);
}

std::vector<uint32_t> FilamentMapper::effective_tool_colors(const std::vector<GcodeToolInfo>& tools,
                                                            const std::vector<AvailableSlot>& slots,
                                                            bool auto_color_map) {
    if (tools.empty()) {
        return {};
    }
    return effective_tool_colors(tools, effective_mappings(tools, slots, auto_color_map), slots);
}

std::vector<uint32_t>
FilamentMapper::effective_tool_colors(const std::vector<GcodeToolInfo>& tools,
                                      const std::vector<ToolMapping>& mappings,
                                      const std::vector<AvailableSlot>& slots) {
    if (tools.empty()) {
        return {};
    }

    // Align @p mappings to @p tools by tool_index — the card's mappings are not
    // guaranteed parallel to the used-tool list, and resolve_display_colors pairs
    // by position. A tool with no matching mapping stays default (unmapped) so it
    // resolves to its own slicer color.
    std::vector<ToolMapping> aligned;
    aligned.reserve(tools.size());
    for (const auto& t : tools) {
        ToolMapping picked;
        for (const auto& m : mappings) {
            if (m.tool_index == t.tool_index) {
                picked = m;
                break;
            }
        }
        aligned.push_back(picked);
    }
    auto per_tool = resolve_display_colors(tools, aligned, slots); // in `tools` order

    // Scatter the tools-ordered colors into a dense vector indexed by logical
    // tool number, so a print that uses e.g. only T0 and T2 lands T2's color at
    // index 2 (the gcode viewer's tool_colors_ is tool-number-indexed). Tool
    // numbers no used tool covers stay the neutral default.
    int max_tool = -1;
    for (const auto& t : tools) {
        max_tool = std::max(max_tool, t.tool_index);
    }
    if (max_tool < 0) {
        return {};
    }

    std::vector<uint32_t> out(static_cast<size_t>(max_tool) + 1, 0x808080);
    for (size_t i = 0; i < tools.size() && i < per_tool.size(); ++i) {
        int idx = tools[i].tool_index;
        if (idx >= 0 && idx <= max_tool) {
            out[static_cast<size_t>(idx)] = per_tool[i];
        }
    }
    return out;
}

std::vector<ToolMapping>
FilamentMapper::use_current_assignments(const std::vector<GcodeToolInfo>& tools,
                                        const std::vector<AvailableSlot>& slots) {
    std::vector<ToolMapping> mappings;
    mappings.reserve(tools.size());

    // Positional assignment: T0→first slot, T1→second slot, etc.
    // No color matching, no rearranging — just use slots in order.
    for (size_t i = 0; i < tools.size(); ++i) {
        ToolMapping mapping;
        mapping.tool_index = tools[i].tool_index;

        if (i < slots.size()) {
            const auto& slot = slots[i];
            mapping.mapped_slot = slot.slot_index;
            mapping.mapped_backend = slot.backend_index;
            mapping.reason = ToolMapping::MatchReason::FIRMWARE_MAPPING;

            if (!tools[i].material.empty() && !slot.material.empty() &&
                !materials_match(tools[i].material, slot.material)) {
                mapping.material_mismatch = true;
            }
        } else {
            mapping.is_auto = true;
            mapping.reason = ToolMapping::MatchReason::AUTO;
        }

        mappings.push_back(mapping);
    }

    return mappings;
}

std::vector<int> FilamentMapper::find_unresolved_tools(const std::vector<ToolMapping>& mappings) {
    std::vector<int> unresolved;
    for (const auto& m : mappings) {
        if (m.is_auto && m.reason == ToolMapping::MatchReason::AUTO) {
            unresolved.push_back(m.tool_index);
        }
    }
    return unresolved;
}

} // namespace helix
