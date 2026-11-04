// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_toolhead_badge.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "helix/ui/text_metrics.h"
#include "theme_manager.h"

#include <cstring>

namespace helix::ui {

namespace {

// Glyph extents beside and below the nozzle center, in tenths of the drawn
// scale, rounded up. Measured from each renderer's drawn pixels; the top edge
// is exact, restated from the renderer's highest design coordinate.
struct Extent {
    int32_t left, right, bottom;
};

constexpr Extent extent_of(ToolheadStyle style) {
    switch (style) {
    case ToolheadStyle::A4T:
        return {19, 19, 35};
    case ToolheadStyle::ANTHEAD:
        return {19, 19, 31};
    case ToolheadStyle::JABBERWOCKY:
        return {25, 16, 36};
    case ToolheadStyle::STEALTHBURNER:
        return {22, 21, 43};
    case ToolheadStyle::CREALITY_K1:
        return {19, 19, 29};
    case ToolheadStyle::CREALITY_K2:
        return {13, 13, 27};
    default:
        return {21, 21, 26};
    }
}

int32_t tenths_up(int32_t scale, int32_t tenths) {
    return (scale * tenths + 9) / 10;
}

// Each case restates its renderer's top edge: the polygon styles map their
// highest design coordinate, the isometric bodies add their cap and iso-top
// offset above the body, and AntHead is the scaled image's top.
int32_t top_of(ToolheadStyle style, int32_t nozzle_y, int32_t s) {
    switch (style) {
    case ToolheadStyle::A4T:
        return nozzle_y + (int32_t)((0 - 630) * ((float)(s * 10) / 2000.0f));
    case ToolheadStyle::STEALTHBURNER:
        return nozzle_y + (int32_t)((78 - 500) * ((float)(s * 10) / 1000.0f));
    case ToolheadStyle::JABBERWOCKY:
        return nozzle_y + (int32_t)((2 - 687) * ((float)(s * 10) / 2400.0f));
    case ToolheadStyle::ANTHEAD:
        return nozzle_y - (81 * ((s * 65) / 10)) / 163;
    case ToolheadStyle::CREALITY_K1:
        return nozzle_y - (s * 48) / 10 / 2 - (s * 6) / 10 / 2;
    case ToolheadStyle::CREALITY_K2:
        return nozzle_y - (s * 48) / 10 / 2 - (s * 5) / 10 / 2;
    default: {
        // Default body plus its raised cap and bevel (each a tenth of the body).
        const int32_t body_height = s * 4;
        const int32_t cap_height = body_height / 10;
        const int32_t body_depth = (s * 6) / 10;
        return nozzle_y - body_height / 2 - 2 * cap_height - body_depth / 2;
    }
    }
}

int32_t token(const char* name) {
    return theme_manager_get_spacing(name);
}

} // namespace

GlyphBounds toolhead_bounds(ToolheadStyle style, int32_t cx, int32_t nozzle_y, int32_t scale) {
#if defined(HELIX_PLATFORM_ESP32)
    // The AntHead image is not shipped; the default glyph is drawn in its place.
    if (style == ToolheadStyle::ANTHEAD)
        style = ToolheadStyle::DEFAULT;
#endif
    const Extent e = extent_of(style);
    return {cx - tenths_up(scale, e.left), top_of(style, nozzle_y, scale),
            cx + tenths_up(scale, e.right), nozzle_y + tenths_up(scale, e.bottom)};
}

ToolBadgeColors tool_badge_colors(bool is_override) {
    const lv_color_t bg = theme_manager_get_color(is_override ? "warning" : "text_muted");
    // Both fills are accents, so the label starts from the palette text colour
    // and shifts toward its pole as 4:1 needs.
    return {bg, theme_manager_get_contrast_adjusted_text(theme_manager_get_color("text"), bg)};
}

ToolBadgeLook tool_badge_look(bool is_override) {
    ToolBadgeLook look{};
    look.height = token("ams_slot_tool_badge_h");
    look.min_width = token("ams_slot_tool_badge_min_w");
    look.pad_h = token("ams_slot_badge_pad_h");
    look.radius = token("ams_slot_tool_badge_radius");
    look.inset = token("ams_slot_tool_badge_inset");
    look.bg_opa = (lv_opa_t)LV_CLAMP(0, token("ams_slot_tool_badge_opa"), 255);
    // text_small's font
    const char* font_name = lv_xml_get_const(nullptr, "font_small");
    look.font = font_name ? lv_xml_get_font(nullptr, font_name) : nullptr;
    look.colors = tool_badge_colors(is_override);
    return look;
}

lv_area_t toolhead_badge_rect(const GlyphBounds& glyph, int32_t width, int32_t height,
                              int32_t inset, BadgeCorner corner) {
    // Like the spool badge on its spool, the badge overhangs the corner: a
    // third of its width and half its height over the glyph, never reaching
    // the nozzle's center column.
    const int32_t mid = (glyph.left + glyph.right) / 2;
    if (corner == BadgeCorner::UpperLeft) {
        const int32_t x2 = LV_MIN(glyph.left + width / 3, mid - inset - 1);
        const int32_t y2 = glyph.top + height / 2;
        return {x2 - width + 1, y2 - height + 1, x2, y2};
    }
    const int32_t x1 = LV_MAX(glyph.right - width / 3, mid + inset + 1);
    const int32_t y1 = glyph.bottom - height / 2;
    return {x1, y1, x1 + width - 1, y1 + height - 1};
}

bool badge_clears_tube(const lv_area_t& badge, int32_t cx, int32_t tube_end_y, int32_t tube_half) {
    const bool beside = badge.x2 < cx - tube_half || badge.x1 > cx + tube_half;
    return beside || badge.y1 > tube_end_y;
}

int32_t tool_badge_width(const ToolBadgeLook& look, const char* label) {
    const int32_t text = (label && look.font) ? text_width(label, look.font) : 0;
    return LV_MAX(look.min_width, text + 2 * look.pad_h);
}

void draw_toolhead_badge(lv_layer_t* layer, const ToolBadgeLook& look, const GlyphBounds& glyph,
                         const char* label) {
    if (!label || !label[0] || !look.font)
        return;
    const lv_area_t area =
        toolhead_badge_rect(glyph, tool_badge_width(look, label), look.height, look.inset);

    lv_draw_fill_dsc_t fill;
    lv_draw_fill_dsc_init(&fill);
    fill.color = look.colors.bg;
    fill.opa = look.bg_opa;
    fill.radius = look.radius;
    lv_draw_fill(layer, &fill, &area);

    lv_draw_label_dsc_t text;
    lv_draw_label_dsc_init(&text);
    text.color = look.colors.text;
    text.font = look.font;
    text.align = LV_TEXT_ALIGN_CENTER;
    text.text = label;
    text.text_local = 1;
    const int32_t font_h = lv_font_get_line_height(look.font);
    const int32_t y1 = area.y1 + (lv_area_get_height(&area) - font_h) / 2;
    const lv_area_t text_area = {area.x1, y1, area.x2, y1 + font_h - 1};
    lv_draw_label(layer, &text, &text_area);
}

} // namespace helix::ui
