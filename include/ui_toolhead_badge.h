// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file ui_toolhead_badge.h
 * @brief A toolhead glyph's bounds and the tool badge superimposed on it.
 *
 * The badge has the spool tool badge's look (ams_slot_view.xml "tool_badge"):
 * both read the ams_slot_tool_badge_* tokens and tool_badge_colors(). On a
 * toolhead it sits in one corner of the glyph for every nozzle style, the
 * corner the filament tube never reaches.
 */

#include "lvgl/lvgl.h"
#include "settings_manager.h"

namespace helix::ui {

/// Drawn extent of a toolhead glyph, inclusive, in display coordinates.
struct GlyphBounds {
    int32_t left, top, right, bottom;
};

/// Bounds of @p style's glyph drawn centered at (@p cx, @p nozzle_y) at
/// @p scale. ANTHEAD reports the default glyph on ESP32, which draws it there.
GlyphBounds toolhead_bounds(ToolheadStyle style, int32_t cx, int32_t nozzle_y, int32_t scale);

/// The nozzle row at which @p style's glyph, drawn at @p scale, ends on @p bottom_y.
/// A layout that stands its head on a line keeps every head's art inside the
/// space above that line, whatever the head's proportions.
int32_t toolhead_nozzle_standing_on(ToolheadStyle style, int32_t bottom_y, int32_t scale);

/// Where filament meets a head: @p dx shifts the head sideways so its entry sits on the
/// filament column it is centered on, and @p y is the entry's row.
struct ToolheadInlet {
    int32_t dx, y;
};
ToolheadInlet toolhead_inlet(ToolheadStyle style, int32_t nozzle_y, int32_t scale);

struct ToolBadgeColors {
    lv_color_t bg, text;
};
/// A tool badge's fill (warning for a user override, else muted) and its
/// contrast-adjusted label color.
ToolBadgeColors tool_badge_colors(bool is_override);

struct ToolBadgeLook {
    int32_t height, min_width, pad_h, radius, inset;
    lv_opa_t bg_opa;
    const lv_font_t* font;
    ToolBadgeColors colors;
};
/// The spool tool badge's look at the current breakpoint and theme.
ToolBadgeLook tool_badge_look(bool is_override = false);

enum class BadgeCorner : uint8_t { UpperLeft, LowerRight };

/// The corner every toolhead badge takes. The filament tube enters a glyph at
/// its top center, and an upper-left badge reaches across that column on the
/// narrower styles.
inline constexpr BadgeCorner kToolheadBadgeCorner = BadgeCorner::LowerRight;

/// The badge rect overhanging @p corner of the glyph: a third of its width and
/// half its height over the glyph, kept @p inset clear of the glyph's center
/// column.
lv_area_t toolhead_badge_rect(const GlyphBounds& glyph, int32_t width, int32_t height,
                              int32_t inset, BadgeCorner corner = kToolheadBadgeCorner);

/// True when @p badge stays off the tube column entering the glyph: x within
/// @p tube_half of @p cx, from the top of the canvas down to @p tube_end_y.
bool badge_clears_tube(const lv_area_t& badge, int32_t cx, int32_t tube_end_y, int32_t tube_half);

/// Badge width for @p label: its text plus padding, at least the minimum width.
int32_t tool_badge_width(const ToolBadgeLook& look, const char* label);

/// Draw @p label's badge on the glyph's corner, at full opacity whatever the
/// glyph's dim. Paint it after every glyph, so a neighbour never covers it.
void draw_toolhead_badge(lv_layer_t* layer, const ToolBadgeLook& look, const GlyphBounds& glyph,
                         const char* label);

} // namespace helix::ui
