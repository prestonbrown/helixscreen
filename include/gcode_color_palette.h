// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lvgl/lvgl.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace helix {
namespace gcode {

/// Shared per-tool color palette used by both 2D and 3D G-code renderers.
/// Converts hex color strings from gcode metadata into lv_color_t values
/// and resolves per-segment colors with optional external override support.
struct GCodeColorPalette {
    std::vector<lv_color_t> tool_colors; ///< From gcode metadata (one per tool)
    lv_color_t override_color{};         ///< External override (AMS/Spoolman)
    bool has_override = false;

    /// Resolve color for a given tool index.
    /// Priority: per-tool color > single override > fallback.
    /// When set_tool_color_overrides() populates tool_colors with AMS slot colors,
    /// those take precedence. Single override is for legacy single-tool path.
    lv_color_t resolve(int tool_index, lv_color_t fallback) const {
        if (tool_index >= 0 && tool_index < static_cast<int>(tool_colors.size())) {
            return tool_colors[static_cast<size_t>(tool_index)];
        }
        if (has_override) {
            return override_color;
        }
        return fallback;
    }

    /// Populate tool_colors from hex strings (e.g., "#ED1C24")
    void set_from_hex_palette(const std::vector<std::string>& hex_colors) {
        tool_colors.clear();
        tool_colors.reserve(hex_colors.size());
        for (const auto& hex : hex_colors) {
            if (hex.size() >= 2 && hex[0] == '#') {
                auto val = static_cast<uint32_t>(std::strtol(hex.c_str() + 1, nullptr, 16));
                tool_colors.push_back(lv_color_hex(val));
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

    /// Check if palette has any tool colors
    bool has_tool_colors() const {
        return !tool_colors.empty();
    }
};

} // namespace gcode
} // namespace helix
