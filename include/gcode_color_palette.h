// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "color_utils.h"

#include <lvgl/lvgl.h>

#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace helix {
namespace gcode {

/// Shared per-tool color palette used by both 2D and 3D G-code renderers.
/// Converts hex color strings from gcode metadata into lv_color_t values
/// and resolves per-segment colors with optional external override support.
///
/// SLOT-ALIGNED: index i is tool i, always. The gcode metadata palette is
/// slot-aligned by contract (gcode_color_metadata.h) — "#A;;#B" parses to
/// {"#A", "", "#B"} so an unknown middle tool keeps its slot. An entry is
/// therefore std::optional: engaged means "the slicer named this tool's color",
/// disengaged means "slot exists, color unknown" and resolve() falls through to
/// the override or the caller's fallback.
///
/// Storing a plain lv_color_t and skipping empty slots (what this did before)
/// compacted the vector, so every tool after a gap resolved to its NEIGHBOUR's
/// color while the 3D builder — which indexes the original strings — painted
/// the same file correctly. Two renderers, two answers.
///
/// The optional also makes the two direct-write paths safe by construction:
/// resize() introduces disengaged (unknown) slots rather than black ones, and
/// any `tool_colors[i] = lv_color_hex(...)` assignment is engaged by definition.
struct GCodeColorPalette {
    /// From gcode metadata, one slot per tool; disengaged = slot with no color
    std::vector<std::optional<lv_color_t>> tool_colors;
    lv_color_t override_color{}; ///< External override (AMS/Spoolman)
    bool has_override = false;

    /// Resolve color for a given tool index.
    /// Priority: per-tool color > single override > fallback.
    /// When set_tool_color_overrides() populates tool_colors with AMS slot colors,
    /// those take precedence. Single override is for legacy single-tool path.
    lv_color_t resolve(int tool_index, lv_color_t fallback) const {
        if (tool_index >= 0 && tool_index < static_cast<int>(tool_colors.size())) {
            const auto& slot = tool_colors[static_cast<size_t>(tool_index)];
            if (slot.has_value()) {
                return *slot;
            }
            // Slot exists but the slicer named no color for it. Fall through to
            // the same answer an out-of-range tool gets — never to a neighbour.
        }
        if (has_override) {
            return override_color;
        }
        return fallback;
    }

    /// Populate tool_colors from hex strings (e.g., "#ED1C24").
    ///
    /// One slot per input entry, ALWAYS — an empty or malformed entry becomes a
    /// disengaged slot, not a missing one, so index i stays tool i. Parsing goes
    /// through helix::parse_hex_color so an 8-digit #RRGGBBAA token means here
    /// exactly what it means everywhere else.
    void set_from_hex_palette(const std::vector<std::string>& hex_colors) {
        tool_colors.clear();
        tool_colors.reserve(hex_colors.size());
        for (const auto& hex : hex_colors) {
            uint32_t rgb = 0;
            if (!hex.empty() && parse_hex_color(hex.c_str(), rgb)) {
                tool_colors.emplace_back(lv_color_hex(rgb));
            } else {
                tool_colors.emplace_back(std::nullopt);
            }
        }
    }

    /// Overlay externally-resolved per-tool colors (AMS lanes / Spoolman) onto
    /// the slicer palette, indexed by logical tool number.
    ///
    /// GROWS the palette to fit @p overrides but never SHRINKS it: an override
    /// vector covering only the tools a print actually uses must not delete
    /// what the slicer said about the others. Truncating instead (the previous
    /// `resize(overrides.size())`) dropped every tool at or above the override
    /// count back to the renderer's single fallback color — which streaming
    /// init had set to filament_palette[initial_tool_index], i.e. T0's. That is
    /// how a 4-tool print rendered entirely in T0's filament once a 2-entry
    /// override arrived.
    ///
    /// Does NOT clear has_override; the caller decides that, since a single
    /// external color and a per-tool map mean different things to it.
    void apply_overrides(const std::vector<uint32_t>& overrides) {
        if (overrides.size() > tool_colors.size()) {
            tool_colors.resize(overrides.size());
        }
        for (size_t i = 0; i < overrides.size(); ++i) {
            tool_colors[i] = lv_color_hex(overrides[i]);
        }
    }

    /// Check if the palette has any slots at all.
    ///
    /// Deliberately "has slots", not "has a known color in some slot": this
    /// guards per-segment render loops, so it must stay O(1). A slot with no
    /// color still resolves correctly — resolve() falls through to the caller's
    /// fallback, which is the same color the guard's else-branch would use.
    bool has_tool_colors() const {
        return !tool_colors.empty();
    }
};

} // namespace gcode
} // namespace helix
