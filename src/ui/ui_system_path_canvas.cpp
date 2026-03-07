// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_system_path_canvas.h"

#include "ui_fonts.h"
#include "ui_spool_drawing.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl/lvgl.h"
#include "nozzle_renderer_a4t.h"
#include "nozzle_renderer_bambu.h"
#include "nozzle_renderer_faceted.h"
#include "settings_manager.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <unordered_map>

// ============================================================================
// Constants
// ============================================================================

// Default dimensions
static constexpr int32_t DEFAULT_WIDTH = 300;
static constexpr int32_t DEFAULT_HEIGHT = 150;

// Layout ratios (as fraction of widget height)
static constexpr float ENTRY_Y_RATIO = 0.05f;    // Top entry points for unit outputs
static constexpr float MERGE_Y_RATIO = 0.25f;    // Where unit lines converge to center
static constexpr float HUB_Y_RATIO = 0.40f;      // Hub center
static constexpr float HUB_HEIGHT_RATIO = 0.10f; // Hub box height
static constexpr float TOOLS_Y_RATIO = 0.62f;    // Tool nozzle row (multi-tool mode)
static constexpr float NOZZLE_Y_RATIO = 0.72f;   // Nozzle center (well below hub, above bottom)

// ============================================================================
// Widget State
// ============================================================================

struct SystemPathData {
    int unit_count = 0;
    static constexpr int MAX_UNITS = 8;
    static constexpr int MAX_TOOLS = 16;
    int32_t unit_x_positions[MAX_UNITS] = {}; // X center of each unit card
    int active_unit = -1;                     // -1 = none active
    uint32_t active_color = 0x4488FF;         // Filament color of active path
    bool filament_loaded = false;             // Whether filament reaches nozzle
    char status_text[64] = {};                // Status label drawn to left of nozzle

    // Bypass support
    bool has_bypass = false;          // Whether to show bypass path
    bool bypass_active = false;       // Whether bypass is the active path (current_slot == -2)
    uint32_t bypass_color = 0x888888; // Color when bypass active

    // Bypass spool state (for spool box rendering)
    bool bypass_has_spool = false;

    // Bypass click callback
    system_path_bypass_cb_t bypass_callback = nullptr;
    void* bypass_user_data = nullptr;

    // Cached bypass spool box position (for click hit-testing)
    int32_t bypass_spool_x = 0;
    int32_t bypass_spool_y = 0;
    int32_t cached_sensor_r = 0;

    // Per-unit hub sensor states
    bool unit_hub_triggered[MAX_UNITS] = {};  // Per-unit hub sensor state
    bool unit_has_hub_sensor[MAX_UNITS] = {}; // Per-unit hub sensor capability

    // Toolhead sensor state
    bool has_toolhead_sensor = false;       // System has a toolhead entry sensor
    bool toolhead_sensor_triggered = false; // Filament detected at toolhead

    // Per-unit tool routing (mixed topology support)
    int unit_tool_count[MAX_UNITS] = {};     // Tools per unit (BT=4, OpenAMS=1)
    int unit_first_tool[MAX_UNITS] = {};     // First tool index for this unit
    int unit_topology[MAX_UNITS] = {};       // 0=LINEAR, 1=HUB, 2=PARALLEL
    int total_tools = 0;                     // Total tool count across all units
    int active_tool = -1;                    // Currently active tool (-1=none)
    int current_tool = -1;                   // Virtual tool number (slot-based, for label)
    int tool_virtual_number[MAX_TOOLS] = {}; // Virtual tool labels per physical nozzle
    bool has_virtual_numbers = false;        // When false, raw physical index is used for labels
    char tool_labels[MAX_TOOLS][8] = {};     // Pre-formatted "Tn" strings for deferred draw
    char current_tool_label[8] = {};         // Pre-formatted label for single-nozzle mode

    // Theme-derived colors (cached)
    lv_color_t color_idle;
    lv_color_t color_hub_bg;
    lv_color_t color_hub_border;
    lv_color_t color_nozzle;
    lv_color_t color_text;

    // Theme-derived sizes
    int32_t line_width_idle = 2;
    int32_t line_width_active = 4;
    int32_t hub_width = 80;
    int32_t hub_height = 30;
    int32_t border_radius = 6;
    int32_t extruder_scale = 10;
    const lv_font_t* label_font = nullptr;

};

// Registry of widget data
static std::unordered_map<lv_obj_t*, SystemPathData*> s_registry;

static SystemPathData* get_data(lv_obj_t* obj) {
    auto it = s_registry.find(obj);
    return (it != s_registry.end()) ? it->second : nullptr;
}

// Load theme-aware colors, fonts, and sizes
static void load_theme_colors(SystemPathData* data) {
    bool dark_mode = theme_manager_is_dark_mode();

    // Try theme-specific tokens first, fall back to standard tokens if they resolve to black
    data->color_idle =
        theme_manager_get_color(dark_mode ? "filament_idle_dark" : "filament_idle_light");
    if (data->color_idle.red == 0 && data->color_idle.green == 0 && data->color_idle.blue == 0) {
        data->color_idle = theme_manager_get_color("text_muted");
    }

    data->color_hub_bg =
        theme_manager_get_color(dark_mode ? "filament_hub_bg_dark" : "filament_hub_bg_light");
    if (data->color_hub_bg.red == 0 && data->color_hub_bg.green == 0 &&
        data->color_hub_bg.blue == 0) {
        data->color_hub_bg = theme_manager_get_color("card_bg");
    }

    data->color_hub_border = theme_manager_get_color(dark_mode ? "filament_hub_border_dark"
                                                               : "filament_hub_border_light");
    if (data->color_hub_border.red == 0 && data->color_hub_border.green == 0 &&
        data->color_hub_border.blue == 0) {
        data->color_hub_border = theme_manager_get_color("border");
    }

    data->color_nozzle = lv_color_hex(0x3A3A3A); // Light charcoal — unloaded nozzle tip

    data->color_text = theme_manager_get_color("text");

    int32_t space_xs = theme_manager_get_spacing("space_xs");
    int32_t space_md = theme_manager_get_spacing("space_md");
    data->line_width_idle = LV_MAX(2, space_xs / 2);
    data->line_width_active = LV_MAX(3, space_xs - 2);
    data->hub_width = LV_MAX(70, space_md * 6);
    data->hub_height = LV_MAX(24, space_md * 2);
    data->border_radius = LV_MAX(4, space_xs);
    data->extruder_scale = LV_MAX(8, space_md);

    const char* font_name = lv_xml_get_const(nullptr, "font_small");
    data->label_font = font_name ? lv_xml_get_font(nullptr, font_name) : &noto_sans_12;

    spdlog::trace("[SystemPath] Theme colors loaded (dark={})", dark_mode);
}

// ============================================================================
// Drawing Helpers
// ============================================================================

// Color manipulation — use shared utilities from ui_spool_drawing.h
static inline lv_color_t sp_darken(lv_color_t c, uint8_t amt) {
    return ui_color_darken(c, amt);
}
static inline lv_color_t sp_lighten(lv_color_t c, uint8_t amt) {
    return ui_color_lighten(c, amt);
}

static void draw_flat_line(lv_layer_t* layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                           lv_color_t color, int32_t width, bool cap_start = true,
                           bool cap_end = true) {
    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = color;
    line_dsc.width = width;
    line_dsc.p1.x = x1;
    line_dsc.p1.y = y1;
    line_dsc.p2.x = x2;
    line_dsc.p2.y = y2;
    line_dsc.round_start = cap_start;
    line_dsc.round_end = cap_end;
    lv_draw_line(layer, &line_dsc);
}

// 3D tube effect: shadow → body → highlight (same approach as filament_path_canvas)
static void draw_tube_line(lv_layer_t* layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                           lv_color_t color, int32_t width, bool cap_start = true,
                           bool cap_end = true) {
    // Shadow: wider, darker
    int32_t shadow_extra = LV_MAX(2, width / 2);
    lv_color_t shadow_color = sp_darken(color, 35);
    draw_flat_line(layer, x1, y1, x2, y2, shadow_color, width + shadow_extra, cap_start, cap_end);

    // Body
    draw_flat_line(layer, x1, y1, x2, y2, color, width, cap_start, cap_end);

    // Highlight: narrower, lighter, offset toward top-left light source
    int32_t hl_width = LV_MAX(1, width * 2 / 5);
    lv_color_t hl_color = sp_lighten(color, 44);

    int32_t dx = x2 - x1;
    int32_t dy = y2 - y1;
    int32_t offset_x = 0;
    int32_t offset_y = 0;
    if (dx == 0) {
        offset_x = (width / 4 + 1);
    } else if (dy == 0) {
        offset_y = -(width / 4 + 1);
    } else {
        float len = sqrtf((float)(dx * dx + dy * dy));
        float px = -(float)dy / len;
        float py = (float)dx / len;
        if (px + py > 0) {
            px = -px;
            py = -py;
        }
        int32_t off_amount = width / 4 + 1;
        offset_x = (int32_t)(px * off_amount);
        offset_y = (int32_t)(py * off_amount);
    }

    draw_flat_line(layer, x1 + offset_x, y1 + offset_y, x2 + offset_x, y2 + offset_y, hl_color,
                   hl_width, cap_start, cap_end);
}

static void draw_vertical_line(lv_layer_t* layer, int32_t x, int32_t y1, int32_t y2,
                               lv_color_t color, int32_t width, bool cap_start = true,
                               bool cap_end = true) {
    draw_tube_line(layer, x, y1, x, y2, color, width, cap_start, cap_end);
}

static void draw_line(lv_layer_t* layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                      lv_color_t color, int32_t width) {
    draw_tube_line(layer, x1, y1, x2, y2, color, width);
}

// Curved tube drawing (cubic bezier) — S-curve routing for clean entry angles
static constexpr int CURVE_SEGMENTS = 16;

// Layer-by-layer curved tube for smooth joints (no visible segment boundaries)
// Uses cubic bezier with two control points for S-curve shaping:
//   CP1 controls departure angle (below start → departs downward)
//   CP2 controls arrival angle (above end → arrives from above)
static void draw_curved_tube(lv_layer_t* layer, int32_t x0, int32_t y0, int32_t cx1, int32_t cy1,
                             int32_t cx2, int32_t cy2, int32_t x1, int32_t y1, lv_color_t color,
                             int32_t width, bool cap_start = true, bool cap_end = true) {
    struct Pt {
        int32_t x, y;
    };
    Pt pts[CURVE_SEGMENTS + 1];
    pts[0] = {x0, y0};
    for (int i = 1; i <= CURVE_SEGMENTS; i++) {
        float t = (float)i / CURVE_SEGMENTS;
        float inv = 1.0f - t;
        float b0 = inv * inv * inv;
        float b1 = 3.0f * inv * inv * t;
        float b2 = 3.0f * inv * t * t;
        float b3 = t * t * t;
        pts[i] = {(int32_t)(b0 * x0 + b1 * cx1 + b2 * cx2 + b3 * x1),
                  (int32_t)(b0 * y0 + b1 * cy1 + b2 * cy2 + b3 * y1)};
    }

    // Round caps between interior segments (overdraw is invisible since same color).
    // Optionally suppress start/end caps at junction with adjacent straight segments.
    // Pass 1: Shadow
    int32_t shadow_extra = LV_MAX(2, width / 2);
    lv_color_t shadow_color = sp_darken(color, 35);
    for (int i = 0; i < CURVE_SEGMENTS; i++) {
        bool cs = (i == 0) ? cap_start : true;
        bool ce = (i == CURVE_SEGMENTS - 1) ? cap_end : true;
        draw_flat_line(layer, pts[i].x, pts[i].y, pts[i + 1].x, pts[i + 1].y, shadow_color,
                       width + shadow_extra, cs, ce);
    }

    // Pass 2: Body
    for (int i = 0; i < CURVE_SEGMENTS; i++) {
        bool cs = (i == 0) ? cap_start : true;
        bool ce = (i == CURVE_SEGMENTS - 1) ? cap_end : true;
        draw_flat_line(layer, pts[i].x, pts[i].y, pts[i + 1].x, pts[i + 1].y, color, width, cs, ce);
    }

    // Pass 3: Highlight
    int32_t hl_width = LV_MAX(1, width * 2 / 5);
    lv_color_t hl_color = sp_lighten(color, 44);
    int32_t dx = x1 - x0;
    int32_t dy = y1 - y0;
    int32_t offset_x = 0;
    int32_t offset_y = 0;
    if (dx == 0) {
        offset_x = (width / 4 + 1);
    } else if (dy == 0) {
        offset_y = -(width / 4 + 1);
    } else {
        float len = sqrtf((float)(dx * dx + dy * dy));
        float px = -(float)dy / len;
        float py = (float)dx / len;
        if (px + py > 0) {
            px = -px;
            py = -py;
        }
        int32_t off_amount = width / 4 + 1;
        offset_x = (int32_t)(px * off_amount);
        offset_y = (int32_t)(py * off_amount);
    }
    for (int i = 0; i < CURVE_SEGMENTS; i++) {
        bool cs = (i == 0) ? cap_start : true;
        bool ce = (i == CURVE_SEGMENTS - 1) ? cap_end : true;
        draw_flat_line(layer, pts[i].x + offset_x, pts[i].y + offset_y, pts[i + 1].x + offset_x,
                       pts[i + 1].y + offset_y, hl_color, hl_width, cs, ce);
    }
}

// ============================================================================
// Routed Tube Drawing (pipe-style: vertical → arc → horizontal → arc → vertical)
// ============================================================================
// Draws a plumbing-style routed path with perfectly vertical entry/exit and
// smooth quarter-circle arc transitions to a horizontal run.

static constexpr int ARC_STEPS = 8;

struct RoutePt {
    int32_t x, y;
};

// Build the point array for a routed path
static int build_routed_path(RoutePt* pts, int32_t sx, int32_t sy, int32_t ex, int32_t ey,
                             int32_t horiz_y, int32_t arc_r) {
    int n = 0;

    if (sx == ex) {
        pts[n++] = {sx, sy};
        pts[n++] = {ex, ey};
        return n;
    }

    bool going_right = (ex > sx);
    int32_t dir = going_right ? 1 : -1;

    // Clamp arc radius to available space
    int32_t horiz_space = going_right ? (ex - sx) : (sx - ex);
    int32_t vert_space_top = horiz_y - sy;
    int32_t vert_space_bot = ey - horiz_y;
    arc_r = LV_MIN(arc_r, horiz_space / 2);
    arc_r = LV_MIN(arc_r, vert_space_top);
    arc_r = LV_MIN(arc_r, vert_space_bot);
    arc_r = LV_MAX(arc_r, 2);

    // Start
    pts[n++] = {sx, sy};

    // End of first vertical
    pts[n++] = {sx, horiz_y - arc_r};

    // First arc: vertical → horizontal
    float cx1 = (float)(sx + dir * arc_r);
    float cy1 = (float)(horiz_y - arc_r);
    float a1_start = going_right ? (float)M_PI : 0.0f;
    float a1_end = (float)(M_PI / 2.0);

    for (int s = 1; s <= ARC_STEPS; s++) {
        float t = (float)s / ARC_STEPS;
        float angle = a1_start + t * (a1_end - a1_start);
        pts[n++] = {(int32_t)(cx1 + arc_r * cosf(angle)), (int32_t)(cy1 + arc_r * sinf(angle))};
    }

    // End of horizontal (only if there's actual horizontal distance beyond the arcs)
    int32_t horiz_end_x = ex - dir * arc_r;
    int32_t horiz_start_x = sx + dir * arc_r;
    if ((going_right && horiz_end_x > horiz_start_x + 1) ||
        (!going_right && horiz_end_x < horiz_start_x - 1)) {
        pts[n++] = {horiz_end_x, horiz_y};
    }

    // Second arc: horizontal → vertical
    float cx2 = (float)(ex - dir * arc_r);
    float cy2 = (float)(horiz_y + arc_r);
    float a2_start = (float)(3.0 * M_PI / 2.0);
    float a2_end = going_right ? (float)(2.0 * M_PI) : (float)M_PI;

    for (int s = 1; s <= ARC_STEPS; s++) {
        float t = (float)s / ARC_STEPS;
        float angle = a2_start + t * (a2_end - a2_start);
        pts[n++] = {(int32_t)(cx2 + arc_r * cosf(angle)), (int32_t)(cy2 + arc_r * sinf(angle))};
    }

    // End
    pts[n++] = {ex, ey};

    return n;
}

// Draw 3D tube along a polyline (multi-pass: shadow → body → highlight)
static void draw_tube_polyline(lv_layer_t* layer, const RoutePt* pts, int count, lv_color_t color,
                               int32_t width, bool cap_start = true, bool cap_end = true) {
    if (count < 2)
        return;

    int32_t shadow_extra = LV_MAX(2, width / 2);
    lv_color_t shadow_color = sp_darken(color, 35);
    for (int i = 0; i < count - 1; i++) {
        bool cs = (i == 0) ? cap_start : true;
        bool ce = (i == count - 2) ? cap_end : true;
        draw_flat_line(layer, pts[i].x, pts[i].y, pts[i + 1].x, pts[i + 1].y, shadow_color,
                       width + shadow_extra, cs, ce);
    }

    for (int i = 0; i < count - 1; i++) {
        bool cs = (i == 0) ? cap_start : true;
        bool ce = (i == count - 2) ? cap_end : true;
        draw_flat_line(layer, pts[i].x, pts[i].y, pts[i + 1].x, pts[i + 1].y, color, width, cs, ce);
    }

    // Highlight: consistent left offset (light from upper-left)
    int32_t hl_width = LV_MAX(1, width * 2 / 5);
    lv_color_t hl_color = sp_lighten(color, 44);
    int32_t hl_off = width / 4 + 1;
    for (int i = 0; i < count - 1; i++) {
        bool cs = (i == 0) ? cap_start : true;
        bool ce = (i == count - 2) ? cap_end : true;
        draw_flat_line(layer, pts[i].x + hl_off, pts[i].y, pts[i + 1].x + hl_off, pts[i + 1].y,
                       hl_color, hl_width, cs, ce);
    }
}

// Draw a routed tube: vert → arc → horiz → arc → vert
static void draw_routed_tube(lv_layer_t* layer, int32_t sx, int32_t sy, int32_t ex, int32_t ey,
                             int32_t horiz_y, int32_t arc_r, lv_color_t color, int32_t width,
                             bool cap_start = true, bool cap_end = true) {
    constexpr int MAX_PTS = 2 + ARC_STEPS + 1 + ARC_STEPS + 1;
    RoutePt pts[MAX_PTS];
    int n = build_routed_path(pts, sx, sy, ex, ey, horiz_y, arc_r);
    draw_tube_polyline(layer, pts, n, color, width, cap_start, cap_end);
}

// Push-to-connect fitting: shadow/highlight matching tube language
static void draw_sensor_dot(lv_layer_t* layer, int32_t cx, int32_t cy, lv_color_t color,
                            bool filled, int32_t radius) {
    lv_draw_arc_dsc_t arc_dsc;
    lv_draw_arc_dsc_init(&arc_dsc);
    arc_dsc.center.x = cx;
    arc_dsc.center.y = cy;
    arc_dsc.start_angle = 0;
    arc_dsc.end_angle = 360;

    // Shadow at full radius
    arc_dsc.radius = static_cast<uint16_t>(radius);
    arc_dsc.width = static_cast<uint16_t>(radius * 2);
    arc_dsc.color = sp_darken(color, 35);
    lv_draw_arc(layer, &arc_dsc);

    if (filled) {
        int32_t body_r = LV_MAX(1, radius - 1);
        arc_dsc.radius = static_cast<uint16_t>(body_r);
        arc_dsc.width = static_cast<uint16_t>(body_r * 2);
        arc_dsc.color = color;
        lv_draw_arc(layer, &arc_dsc);

        int32_t hl_r = LV_MAX(1, radius / 3);
        int32_t hl_off = LV_MAX(1, radius / 3);
        arc_dsc.center.x = cx + hl_off;
        arc_dsc.center.y = cy - hl_off;
        arc_dsc.radius = static_cast<uint16_t>(hl_r);
        arc_dsc.width = static_cast<uint16_t>(hl_r * 2);
        arc_dsc.color = sp_lighten(color, 44);
        lv_draw_arc(layer, &arc_dsc);
    } else {
        arc_dsc.radius = static_cast<uint16_t>(radius - 1);
        arc_dsc.width = 2;
        arc_dsc.color = color;
        lv_draw_arc(layer, &arc_dsc);
    }
}

static void draw_hub_box(lv_layer_t* layer, int32_t cx, int32_t cy, int32_t width, int32_t height,
                         lv_color_t bg_color, lv_color_t border_color, lv_color_t text_color,
                         const lv_font_t* font, int32_t radius, const char* label) {
    // Background
    lv_draw_fill_dsc_t fill_dsc;
    lv_draw_fill_dsc_init(&fill_dsc);
    fill_dsc.color = bg_color;
    fill_dsc.radius = radius;

    lv_area_t box_area = {cx - width / 2, cy - height / 2, cx + width / 2, cy + height / 2};
    lv_draw_fill(layer, &fill_dsc, &box_area);

    // Border
    lv_draw_border_dsc_t border_dsc;
    lv_draw_border_dsc_init(&border_dsc);
    border_dsc.color = border_color;
    border_dsc.width = 2;
    border_dsc.radius = radius;
    lv_draw_border(layer, &border_dsc, &box_area);

    // Label
    if (label && label[0] && font) {
        lv_draw_label_dsc_t label_dsc;
        lv_draw_label_dsc_init(&label_dsc);
        label_dsc.color = text_color;
        label_dsc.font = font;
        label_dsc.align = LV_TEXT_ALIGN_CENTER;
        label_dsc.text = label;

        int32_t font_h = lv_font_get_line_height(font);
        lv_area_t label_area = {cx - width / 2, cy - font_h / 2, cx + width / 2, cy + font_h / 2};
        lv_draw_label(layer, &label_dsc, &label_area);
    }
}

// Color blending helper (same pattern as filament_path_canvas)
static lv_color_t sp_blend(lv_color_t c1, lv_color_t c2, float factor) {
    factor = LV_CLAMP(factor, 0.0f, 1.0f);
    return lv_color_make((uint8_t)(c1.red + (c2.red - c1.red) * factor),
                         (uint8_t)(c1.green + (c2.green - c1.green) * factor),
                         (uint8_t)(c1.blue + (c2.blue - c1.blue) * factor));
}

/**
 * @brief Draw a tool badge (rounded rect + "Tn" label) beneath a nozzle
 *
 * Replicates the tool_badge style from ams_slot_view.xml using draw primitives.
 * Used for both multi-tool nozzle labels and single-nozzle virtual tool display.
 *
 * @param layer Draw layer
 * @param cx Center X of the nozzle above
 * @param nozzle_y Center Y of the nozzle
 * @param nozzle_scale Scale of the nozzle icon (determines vertical offset)
 * @param label Pre-formatted label string (must remain valid through draw cycle)
 * @param font Label font
 * @param bg_color Badge background color
 * @param text_color Badge text color
 */
static void draw_tool_badge(lv_layer_t* layer, int32_t cx, int32_t nozzle_y, int32_t nozzle_scale,
                            const char* label, const lv_font_t* font, lv_color_t bg_color,
                            lv_color_t text_color) {
    if (!label || !label[0] || !font)
        return;

    const char* tool_label = label;

    int32_t font_h = lv_font_get_line_height(font);
    int32_t label_len = (int32_t)strlen(tool_label);
    // Approximate width: ~60% of font height per character for small labels
    int32_t badge_w = LV_MAX(24, label_len * (font_h * 3 / 5) + 6);
    int32_t badge_h = font_h + 4;
    int32_t badge_top = nozzle_y + nozzle_scale * 4 + 6;
    int32_t badge_left = cx - badge_w / 2;

    // Badge background (rounded rect)
    lv_area_t badge_area = {badge_left, badge_top, badge_left + badge_w, badge_top + badge_h};
    lv_draw_fill_dsc_t fill_dsc;
    lv_draw_fill_dsc_init(&fill_dsc);
    fill_dsc.color = bg_color;
    fill_dsc.opa = 200;
    fill_dsc.radius = 4;
    lv_draw_fill(layer, &fill_dsc, &badge_area);

    // Badge text
    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.color = text_color;
    label_dsc.font = font;
    label_dsc.align = LV_TEXT_ALIGN_CENTER;
    label_dsc.text = tool_label;

    lv_area_t text_area = {badge_left, badge_top + 2, badge_left + badge_w, badge_top + 2 + font_h};
    lv_draw_label(layer, &label_dsc, &text_area);
}

// ============================================================================
// Main Draw Callback
// ============================================================================

// Helper: calculate horizontal X position for a tool in the tools row
static int32_t calc_tool_x(int tool_index, int total_tools, int32_t x_off, int32_t width) {
    if (total_tools <= 1) {
        return x_off + width / 2;
    }
    // Distribute tools evenly across 20%-80% of widget width
    int32_t margin = width / 5;
    int32_t usable = width - 2 * margin;
    if (total_tools == 1) {
        return x_off + width / 2;
    }
    return x_off + margin + (usable * tool_index) / (total_tools - 1);
}

static void system_path_draw_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    SystemPathData* data = get_data(obj);
    if (!data)
        return;

    if (data->unit_count <= 0) {
        spdlog::trace("[SystemPath] No units to draw");
        return;
    }

    // Get widget dimensions
    lv_area_t obj_coords;
    lv_obj_get_coords(obj, &obj_coords);
    int32_t width = lv_area_get_width(&obj_coords);
    int32_t height = lv_area_get_height(&obj_coords);
    int32_t x_off = obj_coords.x1;
    int32_t y_off = obj_coords.y1;

    // Determine if multi-tool routing is needed
    bool multi_tool = (data->total_tools > 1);

    // Calculate Y positions
    int32_t entry_y = y_off + (int32_t)(height * ENTRY_Y_RATIO);
    int32_t merge_y = y_off + (int32_t)(height * MERGE_Y_RATIO);
    int32_t hub_y = y_off + (int32_t)(height * HUB_Y_RATIO);
    int32_t hub_h = (int32_t)(height * HUB_HEIGHT_RATIO);
    int32_t tools_y = y_off + (int32_t)(height * TOOLS_Y_RATIO);
    int32_t nozzle_y = y_off + (int32_t)(height * NOZZLE_Y_RATIO);
    int32_t center_x = x_off + width / 2;

    // Colors
    lv_color_t idle_color = data->color_idle;
    lv_color_t active_color_lv = lv_color_hex(data->active_color);
    lv_color_t hub_bg = data->color_hub_bg;
    lv_color_t hub_border = data->color_hub_border;
    lv_color_t nozzle_color = data->color_nozzle;

    // Sizes
    int32_t line_idle = data->line_width_idle;
    int32_t line_active = data->line_width_active;
    int32_t sensor_r = LV_MAX(5, data->line_width_active);
    data->cached_sensor_r = sensor_r;

    // Shift center_x left when bypass is supported to make room for bypass path on the right
    if (data->has_bypass && !multi_tool) {
        center_x -= width / 10; // Shift hub/toolhead ~10% left
    }

    if (multi_tool) {
        // ====================================================================
        // MULTI-TOOL MODE: Per-unit routing to individual tool positions
        // Note: Bypass rendering is intentionally omitted here — bypass mode
        // is not applicable to multi-extruder toolchanger setups since each
        // tool has its own filament path.
        // ====================================================================

        // ================================================================
        // PASS 1: Collect all routed paths across all units globally
        // ================================================================
        struct GlobalRoute {
            int unit_idx;
            int tool_idx;
            int32_t start_x;
            int32_t start_y;
            int32_t end_x;
            int32_t end_y;
            int32_t dist; // absolute horizontal distance (for stagger ordering)
            bool is_hub;  // HUB topology route (draws hub box after)
        };
        GlobalRoute all_routes[SystemPathData::MAX_TOOLS];
        int total_routes = 0;

        int32_t arc_r = LV_MAX(8, (tools_y - entry_y) / 10);

        // Per-unit hub info for deferred hub box drawing
        struct HubInfo {
            int32_t tool_x;
            int32_t mini_hub_y;
            int32_t mini_hub_w;
            int32_t mini_hub_h;
            lv_color_t hub_bg_color;
            int first_tool;
            bool valid;
        };
        HubInfo hub_infos[SystemPathData::MAX_UNITS] = {};

        for (int i = 0; i < data->unit_count && i < SystemPathData::MAX_UNITS; i++) {
            int32_t unit_x = x_off + data->unit_x_positions[i];
            int topology = data->unit_topology[i];
            int tool_count = data->unit_tool_count[i];
            int first_tool = data->unit_first_tool[i];
            bool is_active = (i == data->active_unit);

            if (topology == 2) {
                // PARALLEL: one route per tool, spread start positions
                int32_t spread = LV_MIN(width / 6, tool_count > 1 ? 60 : 0);
                for (int t = 0; t < tool_count && (first_tool + t) < data->total_tools; ++t) {
                    int tool_idx = first_tool + t;
                    int32_t tool_x = calc_tool_x(tool_idx, data->total_tools, x_off, width);
                    int32_t start_x = unit_x;
                    if (tool_count > 1) {
                        start_x = unit_x - spread / 2 + (spread * t) / (tool_count - 1);
                    }
                    int32_t dist = start_x > tool_x ? (start_x - tool_x) : (tool_x - start_x);
                    all_routes[total_routes++] = {i,      tool_idx, start_x, entry_y,
                                                  tool_x, tools_y,  dist,    false};
                }
            } else {
                // HUB: one route from unit to mini-hub position
                if (tool_count > 0 && first_tool < data->total_tools) {
                    int32_t tool_x = calc_tool_x(first_tool, data->total_tools, x_off, width);
                    int32_t mini_hub_w = data->hub_width * 2 / 3;
                    int32_t mini_hub_h = hub_h * 2 / 3;
                    int32_t mini_hub_y = merge_y + (tools_y - merge_y) / 3;
                    int32_t end_y_mh = mini_hub_y - mini_hub_h / 2;

                    // Hub sensor dot and short vertical beneath it
                    // Use a shorter merge point for HUB units to leave more room
                    // between hub routes and parallel routes below
                    int32_t hub_merge_y = entry_y + (merge_y - entry_y) * 2 / 3;
                    bool has_sensor = data->unit_has_hub_sensor[i];
                    lv_color_t line_color = is_active ? active_color_lv : idle_color;
                    int32_t line_w = is_active ? line_active : line_idle;
                    int32_t sensor_dot_y = entry_y + (hub_merge_y - entry_y) / 3;

                    if (has_sensor) {
                        draw_vertical_line(layer, unit_x, entry_y, sensor_dot_y - sensor_r,
                                           line_color, line_w);
                        draw_vertical_line(layer, unit_x, sensor_dot_y + sensor_r, hub_merge_y,
                                           line_color, line_w, true, /*cap_end=*/false);
                        bool filled = data->unit_hub_triggered[i];
                        lv_color_t dot_color =
                            filled ? (is_active ? active_color_lv : idle_color) : idle_color;
                        draw_sensor_dot(layer, unit_x, sensor_dot_y, dot_color, filled, sensor_r);
                    } else {
                        draw_vertical_line(layer, unit_x, entry_y, hub_merge_y, line_color, line_w,
                                           true, /*cap_end=*/false);
                    }

                    int32_t dist = unit_x > tool_x ? (unit_x - tool_x) : (tool_x - unit_x);
                    all_routes[total_routes++] = {i,      first_tool, unit_x, hub_merge_y,
                                                  tool_x, end_y_mh,   dist,   true};

                    // Save hub info for deferred drawing
                    bool hub_has_filament = is_active && data->filament_loaded;
                    lv_color_t mini_hub_bg = hub_bg;
                    if (hub_has_filament) {
                        mini_hub_bg = sp_blend(hub_bg, active_color_lv, 0.33f);
                    }
                    hub_infos[i] = {tool_x,      mini_hub_y, mini_hub_w, mini_hub_h,
                                    mini_hub_bg, first_tool, true};
                }
            }
        }

        // ================================================================
        // PASS 2: Sort routes. PARALLEL by end_x ascending (leftmost
        // tool first → bottom horizontal). HUB after parallel, by
        // distance descending.
        // ================================================================
        for (int a = 0; a < total_routes - 1; ++a) {
            for (int b = a + 1; b < total_routes; ++b) {
                bool swap = false;
                if (all_routes[a].is_hub && !all_routes[b].is_hub) {
                    swap = true; // parallel before hub
                } else if (all_routes[a].is_hub == all_routes[b].is_hub) {
                    if (!all_routes[a].is_hub) {
                        // PARALLEL: sort by end_x ascending
                        if (all_routes[b].end_x < all_routes[a].end_x)
                            swap = true;
                    } else {
                        // HUB: sort by distance descending
                        if (all_routes[b].dist > all_routes[a].dist)
                            swap = true;
                    }
                }
                if (swap) {
                    GlobalRoute tmp = all_routes[a];
                    all_routes[a] = all_routes[b];
                    all_routes[b] = tmp;
                }
            }
        }

        // ================================================================
        // PASS 3: Draw all routed paths with computed coordinates.
        //
        // PARALLEL geometry (cable harness nesting):
        //   Routes sorted by end_x ascending (T0 leftmost first).
        //   Horizontal levels are fixed-spaced pixel positions centered
        //   in the midzone between entry_y and tools_y.
        //   T0 (first, leftmost end_x) → LOWEST horizontal (highest Y)
        //   T3 (last, rightmost end_x) → HIGHEST horizontal (lowest Y)
        //   This guarantees no crossings: since end_x increases left→right
        //   and horiz_y increases top→bottom in the same order, no end
        //   vertical segment can pass through another route's horizontal.
        //
        // HUB geometry:
        //   20%-40% of own vertical range for clean hub-top arrival.
        // ================================================================

        int parallel_count = 0;
        int hub_count = 0;
        for (int r = 0; r < total_routes; ++r) {
            if (all_routes[r].start_x == all_routes[r].end_x)
                continue;
            if (all_routes[r].is_hub)
                hub_count++;
            else
                parallel_count++;
        }

        // PARALLEL: compute absolute Y positions for each horizontal level
        // Fixed spacing between levels (tube width * 3 gives clear visual gap)
        int32_t par_step = LV_MAX(10, line_idle * 3 + 4);
        // Total height of the stacked group
        int32_t par_group_h = (parallel_count > 1) ? par_step * (parallel_count - 1) : 0;
        // Center the group at 55% between entry_y and tools_y (slightly below middle)
        int32_t par_center_y = entry_y + (tools_y - entry_y) * 55 / 100;
        // Top of group (highest horizontal = smallest Y = last parallel route)
        int32_t par_top_y = par_center_y - par_group_h / 2;
        // Bottom of group (lowest horizontal = largest Y = first parallel route)
        int32_t par_bot_y = par_top_y + par_group_h;

        int parallel_idx = 0;
        int hub_idx = 0;

        for (int r = 0; r < total_routes; ++r) {
            auto& route = all_routes[r];
            bool is_active = (route.unit_idx == data->active_unit);
            bool tool_active = is_active && (route.tool_idx == data->active_tool);

            lv_color_t route_color = tool_active ? active_color_lv : idle_color;
            int32_t route_w = tool_active ? line_active : line_idle;

            if (route.start_x == route.end_x) {
                draw_tube_line(layer, route.start_x, route.start_y, route.end_x, route.end_y,
                               is_active ? active_color_lv : idle_color,
                               is_active ? line_active : line_idle);
            } else if (route.is_hub) {
                // HUB: 20%-40% of own range for clean hub-top arrival
                float f = 0.20f;
                if (hub_count > 1) {
                    f = 0.20f + 0.20f * (float)hub_idx / (float)(hub_count - 1);
                }
                hub_idx++;

                int32_t route_drop = route.end_y - route.start_y;
                int32_t horiz_y = route.start_y + (int32_t)(route_drop * f);
                horiz_y = LV_CLAMP(horiz_y, route.start_y + arc_r + 2, route.end_y - arc_r - 2);

                draw_routed_tube(layer, route.start_x, route.start_y, route.end_x, route.end_y,
                                 horiz_y, arc_r, route_color, route_w,
                                 /*cap_start=*/false);
            } else {
                // PARALLEL: idx 0 (leftmost end_x) at par_bot_y (lowest),
                // idx N-1 (rightmost end_x) at par_top_y (highest)
                int32_t horiz_y = par_bot_y - parallel_idx * par_step;
                parallel_idx++;

                horiz_y = LV_CLAMP(horiz_y, route.start_y + arc_r + 2, route.end_y - arc_r - 2);

                draw_routed_tube(layer, route.start_x, route.start_y, route.end_x, route.end_y,
                                 horiz_y, arc_r, route_color, route_w,
                                 /*cap_start=*/false);
            }
        }

        // ================================================================
        // PASS 4: Draw hub boxes and hub-to-tool verticals (on top of routes)
        // ================================================================
        for (int i = 0; i < data->unit_count && i < SystemPathData::MAX_UNITS; i++) {
            if (!hub_infos[i].valid)
                continue;
            auto& hi = hub_infos[i];
            bool is_active = (i == data->active_unit);

            draw_hub_box(layer, hi.tool_x, hi.mini_hub_y, hi.mini_hub_w, hi.mini_hub_h,
                         hi.hub_bg_color, hub_border, data->color_text, data->label_font,
                         data->border_radius, "Hub");

            // Line from mini hub to tool
            bool tool_active = is_active && (hi.first_tool == data->active_tool);
            lv_color_t out_color = tool_active ? active_color_lv : idle_color;
            int32_t out_w = tool_active ? line_active : line_idle;
            draw_vertical_line(layer, hi.tool_x, hi.mini_hub_y + hi.mini_hub_h / 2, tools_y,
                               out_color, out_w);
        }

        // Draw tool nozzles at the bottom
        int32_t small_scale = LV_MAX(6, data->extruder_scale * 3 / 4);
        for (int t = 0; t < data->total_tools && t < SystemPathData::MAX_TOOLS; ++t) {
            int32_t tool_x = calc_tool_x(t, data->total_tools, x_off, width);
            bool is_active_tool = (t == data->active_tool) && data->filament_loaded;

            lv_color_t noz_color = is_active_tool ? active_color_lv : nozzle_color;
            switch (helix::SettingsManager::instance().get_effective_toolhead_style()) {
                case helix::ToolheadStyle::STEALTHBURNER:
                    draw_nozzle_faceted(layer, tool_x, tools_y, noz_color, small_scale);
                    break;
                case helix::ToolheadStyle::A4T:
                    draw_nozzle_a4t(layer, tool_x, tools_y, noz_color, small_scale);
                    break;
                default:
                    draw_nozzle_bambu(layer, tool_x, tools_y, noz_color, small_scale);
                    break;
            }

            // Tool badge below nozzle — use pre-formatted label from data
            if (data->label_font && t < SystemPathData::MAX_TOOLS) {
                draw_tool_badge(layer, tool_x, tools_y, small_scale, data->tool_labels[t],
                                data->label_font, data->color_idle,
                                is_active_tool ? active_color_lv : data->color_text);
            }
        }

        // Draw status text at the bottom
        if (data->status_text[0] && data->label_font) {
            lv_draw_label_dsc_t status_dsc;
            lv_draw_label_dsc_init(&status_dsc);
            status_dsc.color = data->color_text;
            status_dsc.font = data->label_font;
            status_dsc.align = LV_TEXT_ALIGN_CENTER;
            status_dsc.text = data->status_text;

            int32_t font_h = lv_font_get_line_height(data->label_font);
            int32_t status_y = y_off + height - font_h - 2;
            lv_area_t status_area = {x_off + 4, status_y, x_off + width - 4, status_y + font_h};
            lv_draw_label(layer, &status_dsc, &status_area);
        }

    } else {
        // ====================================================================
        // SINGLE-TOOL MODE: Original hub convergence (unchanged)
        // ====================================================================

        // Draw unit entry lines (one per unit, from entry to merge point)
        for (int i = 0; i < data->unit_count && i < SystemPathData::MAX_UNITS; i++) {
            int32_t unit_x = x_off + data->unit_x_positions[i];
            bool is_active = (i == data->active_unit);

            lv_color_t line_color = is_active ? active_color_lv : idle_color;
            int32_t line_w = is_active ? line_active : line_idle;

            // Hub sensor dot interrupts the vertical segment
            bool has_sensor = data->unit_has_hub_sensor[i];
            int32_t sensor_dot_y = entry_y + (merge_y - entry_y) * 3 / 5;

            // Suppress end cap on last straight segment and start cap on curve
            // to eliminate visible endcap seam at straight→curve junction
            if (has_sensor) {
                draw_vertical_line(layer, unit_x, entry_y, sensor_dot_y - sensor_r, line_color,
                                   line_w);
                draw_vertical_line(layer, unit_x, sensor_dot_y + sensor_r, merge_y, line_color,
                                   line_w, true, /*cap_end=*/false);
                bool filled = data->unit_hub_triggered[i];
                lv_color_t dot_color =
                    filled ? (is_active ? active_color_lv : idle_color) : idle_color;
                draw_sensor_dot(layer, unit_x, sensor_dot_y, dot_color, filled, sensor_r);
            } else {
                draw_vertical_line(layer, unit_x, entry_y, merge_y, line_color, line_w, true,
                                   /*cap_end=*/false);
            }

            // S-curve from unit to hub — CPs at ~86% for vertical ends
            {
                int32_t end_y_hub = hub_y - hub_h / 2;
                int32_t drop = end_y_hub - merge_y;
                draw_curved_tube(layer, unit_x, merge_y, unit_x, merge_y + drop * 6 / 7, center_x,
                                 end_y_hub - drop * 6 / 7, center_x, end_y_hub, line_color, line_w,
                                 /*cap_start=*/false);
            }
        }

        // Draw bypass path (if supported)
        if (data->has_bypass) {
            int32_t hub_right = center_x + data->hub_width / 2;
            int32_t bypass_x = hub_right + width / 8;
            bool bp_active = data->bypass_active;

            lv_color_t bp_color = bp_active ? lv_color_hex(data->bypass_color) : idle_color;
            int32_t bp_width = bp_active ? line_active : line_idle;

            int32_t hub_bottom = hub_y + hub_h / 2;
            int32_t bypass_merge_y = hub_bottom + (nozzle_y - hub_bottom) / 3;

            // Draw spool box above merge point
            int32_t spool_y = bypass_merge_y - sensor_r * 3;
            lv_color_t spool_color =
                data->bypass_has_spool ? lv_color_hex(data->bypass_color) : idle_color;
            ui_draw_spool_box(layer, bypass_x, spool_y, spool_color, data->bypass_has_spool,
                              sensor_r);

            // Cache position for click hit-testing
            data->bypass_spool_x = bypass_x;
            data->bypass_spool_y = spool_y;

            // "Bypass" label above spool box
            if (data->label_font) {
                lv_draw_label_dsc_t bp_label_dsc;
                lv_draw_label_dsc_init(&bp_label_dsc);
                bp_label_dsc.color =
                    bp_active ? lv_color_hex(data->bypass_color) : data->color_text;
                bp_label_dsc.font = data->label_font;
                bp_label_dsc.align = LV_TEXT_ALIGN_CENTER;
                bp_label_dsc.text = "Bypass";

                int32_t font_h = lv_font_get_line_height(data->label_font);
                int32_t label_top = spool_y - sensor_r * 2 - font_h - 2;
                lv_area_t bp_label_area = {bypass_x - 30, label_top, bypass_x + 30,
                                           label_top + font_h};
                lv_draw_label(layer, &bp_label_dsc, &bp_label_area);
            }

            // Vertical line from spool box to merge point
            draw_line(layer, bypass_x, spool_y + sensor_r * 2, bypass_x, bypass_merge_y, bp_color,
                      bp_width);
            // Horizontal line from merge to hub
            draw_line(layer, bypass_x, bypass_merge_y, center_x + sensor_r, bypass_merge_y,
                      bp_color, bp_width);
            draw_sensor_dot(layer, center_x, bypass_merge_y, bp_color, bp_active, sensor_r);
        }

        // Draw combiner hub
        {
            bool hub_has_filament = (data->active_unit >= 0 && data->filament_loaded);
            lv_color_t hub_bg_tinted = hub_bg;
            if (hub_has_filament) {
                hub_bg_tinted = sp_blend(hub_bg, active_color_lv, 0.33f);
            }
            draw_hub_box(layer, center_x, hub_y, data->hub_width, hub_h, hub_bg_tinted, hub_border,
                         data->color_text, data->label_font, data->border_radius, "Hub");
        }

        // Draw output line from hub to nozzle (with sensor dots)
        {
            bool unit_active = (data->active_unit >= 0 && data->filament_loaded);
            bool bp_active = (data->bypass_active && data->filament_loaded);
            bool any_active = unit_active || bp_active;

            int32_t hub_bottom = hub_y + hub_h / 2;
            int32_t extruder_half_height = data->extruder_scale * 2;
            int32_t nozzle_top = nozzle_y - extruder_half_height;
            int32_t bypass_merge_y = hub_bottom + (nozzle_y - hub_bottom) / 3;
            int32_t toolhead_sensor_y = hub_bottom + (nozzle_top - hub_bottom) * 2 / 3;

            lv_color_t active_output_color =
                bp_active ? lv_color_hex(data->bypass_color) : active_color_lv;

            if (bp_active) {
                draw_vertical_line(layer, center_x, hub_bottom, bypass_merge_y, idle_color,
                                   line_idle);
                draw_vertical_line(layer, center_x, bypass_merge_y, nozzle_top,
                                   lv_color_hex(data->bypass_color), line_active);
            } else if (unit_active) {
                draw_vertical_line(layer, center_x, hub_bottom, nozzle_top, active_color_lv,
                                   line_active);
            } else {
                draw_vertical_line(layer, center_x, hub_bottom, nozzle_top, idle_color, line_idle);
            }

            if (data->has_toolhead_sensor) {
                bool th_filled = data->toolhead_sensor_triggered;
                lv_color_t th_dot_color = th_filled ? active_output_color : idle_color;
                if (!any_active)
                    th_dot_color = idle_color;
                draw_sensor_dot(layer, center_x, toolhead_sensor_y, th_dot_color, th_filled,
                                sensor_r);
            }

            lv_color_t noz_color = nozzle_color;
            if (bp_active) {
                noz_color = lv_color_hex(data->bypass_color);
            } else if (unit_active) {
                noz_color = active_color_lv;
            }

            switch (helix::SettingsManager::instance().get_effective_toolhead_style()) {
                case helix::ToolheadStyle::STEALTHBURNER:
                    draw_nozzle_faceted(layer, center_x, nozzle_y, noz_color, data->extruder_scale);
                    break;
                case helix::ToolheadStyle::A4T:
                    draw_nozzle_a4t(layer, center_x, nozzle_y, noz_color, data->extruder_scale);
                    break;
                default:
                    draw_nozzle_bambu(layer, center_x, nozzle_y, noz_color, data->extruder_scale);
                    break;
            }

            // Virtual tool badge beneath nozzle — only when multiple slots feed one toolhead
            if (data->total_tools <= 1 && data->current_tool >= 0 && data->label_font) {
                lv_color_t badge_text = (unit_active || bp_active) ? noz_color : data->color_text;
                draw_tool_badge(layer, center_x, nozzle_y, data->extruder_scale,
                                data->current_tool_label, data->label_font, data->color_idle,
                                badge_text);
            }

            if (data->status_text[0] && data->label_font) {
                lv_draw_label_dsc_t status_dsc;
                lv_draw_label_dsc_init(&status_dsc);
                status_dsc.color = data->color_text;
                status_dsc.font = data->label_font;
                status_dsc.align = LV_TEXT_ALIGN_RIGHT;
                status_dsc.text = data->status_text;

                int32_t font_h = lv_font_get_line_height(data->label_font);
                int32_t label_right = center_x - data->extruder_scale * 3;
                int32_t label_left = x_off + 4;
                lv_area_t status_area = {label_left, nozzle_y - font_h / 2, label_right,
                                         nozzle_y + font_h / 2};
                lv_draw_label(layer, &status_dsc, &status_area);
            }
        }
    }

    spdlog::trace("[SystemPath] Draw: units={}, active={}, loaded={}, tools={}, active_tool={}, "
                  "current_tool={}, bypass={}(active={})",
                  data->unit_count, data->active_unit, data->filament_loaded, data->total_tools,
                  data->active_tool, data->current_tool, data->has_bypass, data->bypass_active);
}

// ============================================================================
// Event Handlers
// ============================================================================

static void on_system_path_clicked(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    auto* data = get_data(obj);
    if (!data || !data->bypass_callback || !data->has_bypass)
        return;

    lv_point_t point;
    lv_indev_t* indev = lv_indev_active();
    if (!indev)
        return;
    lv_indev_get_point(indev, &point);

    // Hit-test bypass spool box (bypass_spool_x/y are in absolute screen coords)
    int32_t sr = data->cached_sensor_r;
    int32_t box_w = sr * 3;
    int32_t box_h = sr * 4;
    if (abs(point.x - data->bypass_spool_x) < box_w &&
        abs(point.y - data->bypass_spool_y) < box_h) {
        spdlog::debug("[SystemPath] Bypass spool box clicked");
        data->bypass_callback(data->bypass_user_data);
        return;
    }
}

static void system_path_delete_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    auto it = s_registry.find(obj);
    if (it != s_registry.end()) {
        std::unique_ptr<SystemPathData> data(it->second);
        s_registry.erase(it);
        // data automatically freed when unique_ptr goes out of scope
    }
}

// ============================================================================
// XML Widget Interface
// ============================================================================

static void* system_path_xml_create(lv_xml_parser_state_t* state, const char** attrs) {
    LV_UNUSED(attrs);

    void* parent = lv_xml_state_get_parent(state);
    lv_obj_t* obj = lv_obj_create(static_cast<lv_obj_t*>(parent));
    if (!obj)
        return nullptr;

    auto data_ptr = std::make_unique<SystemPathData>();
    s_registry[obj] = data_ptr.get();
    auto* data = data_ptr.release();

    // Load theme-aware colors, fonts, and sizes
    load_theme_colors(data);

    // Configure object
    lv_obj_set_size(obj, DEFAULT_WIDTH, DEFAULT_HEIGHT);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);

    // Register event handlers
    lv_obj_add_event_cb(obj, system_path_draw_cb, LV_EVENT_DRAW_POST, nullptr);
    lv_obj_add_event_cb(obj, system_path_delete_cb, LV_EVENT_DELETE, nullptr);
    lv_obj_add_event_cb(obj, on_system_path_clicked, LV_EVENT_CLICKED, nullptr);

    spdlog::debug("[SystemPath] Created widget via XML");
    return obj;
}

static void system_path_xml_apply(lv_xml_parser_state_t* state, const char** attrs) {
    void* item = lv_xml_state_get_item(state);
    lv_obj_t* obj = static_cast<lv_obj_t*>(item);
    if (!obj)
        return;

    lv_xml_obj_apply(state, attrs);

    auto* data = get_data(obj);
    if (!data)
        return;

    bool needs_redraw = false;

    for (int i = 0; attrs[i]; i += 2) {
        const char* name = attrs[i];
        const char* value = attrs[i + 1];

        if (strcmp(name, "unit_count") == 0) {
            data->unit_count = LV_CLAMP(atoi(value), 0, SystemPathData::MAX_UNITS);
            needs_redraw = true;
        } else if (strcmp(name, "active_unit") == 0) {
            data->active_unit = atoi(value);
            needs_redraw = true;
        } else if (strcmp(name, "active_color") == 0) {
            data->active_color = strtoul(value, nullptr, 0);
            needs_redraw = true;
        } else if (strcmp(name, "filament_loaded") == 0) {
            data->filament_loaded = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
            needs_redraw = true;
        }
    }

    if (needs_redraw) {
        lv_obj_invalidate(obj);
    }
}

// ============================================================================
// Public API
// ============================================================================

void ui_system_path_canvas_register(void) {
    lv_xml_register_widget("system_path_canvas", system_path_xml_create, system_path_xml_apply);
    spdlog::info("[SystemPath] Registered system_path_canvas widget with XML system");
}

lv_obj_t* ui_system_path_canvas_create(lv_obj_t* parent) {
    if (!parent) {
        spdlog::error("[SystemPath] Cannot create: parent is null");
        return nullptr;
    }

    lv_obj_t* obj = lv_obj_create(parent);
    if (!obj) {
        spdlog::error("[SystemPath] Failed to create object");
        return nullptr;
    }

    auto data_ptr = std::make_unique<SystemPathData>();
    s_registry[obj] = data_ptr.get();
    auto* data = data_ptr.release();

    // Load theme-aware colors, fonts, and sizes
    load_theme_colors(data);

    // Configure object
    lv_obj_set_size(obj, DEFAULT_WIDTH, DEFAULT_HEIGHT);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);

    // Register event handlers
    lv_obj_add_event_cb(obj, system_path_draw_cb, LV_EVENT_DRAW_POST, nullptr);
    lv_obj_add_event_cb(obj, system_path_delete_cb, LV_EVENT_DELETE, nullptr);
    lv_obj_add_event_cb(obj, on_system_path_clicked, LV_EVENT_CLICKED, nullptr);

    spdlog::debug("[SystemPath] Created widget programmatically");
    return obj;
}

void ui_system_path_canvas_set_unit_count(lv_obj_t* obj, int count) {
    auto* data = get_data(obj);
    if (!data)
        return;
    int clamped = LV_CLAMP(count, 0, SystemPathData::MAX_UNITS);
    if (data->unit_count == clamped)
        return;
    data->unit_count = clamped;
    lv_obj_invalidate(obj);
}

void ui_system_path_canvas_set_unit_x(lv_obj_t* obj, int unit_index, int32_t center_x) {
    auto* data = get_data(obj);
    if (!data || unit_index < 0 || unit_index >= SystemPathData::MAX_UNITS)
        return;
    if (data->unit_x_positions[unit_index] == center_x)
        return;
    data->unit_x_positions[unit_index] = center_x;
    lv_obj_invalidate(obj);
}

void ui_system_path_canvas_set_active_unit(lv_obj_t* obj, int unit_index) {
    auto* data = get_data(obj);
    if (!data || data->active_unit == unit_index)
        return;
    data->active_unit = unit_index;
    lv_obj_invalidate(obj);
}

void ui_system_path_canvas_set_active_color(lv_obj_t* obj, uint32_t color) {
    auto* data = get_data(obj);
    if (!data || data->active_color == color)
        return;
    data->active_color = color;
    lv_obj_invalidate(obj);
}

void ui_system_path_canvas_set_filament_loaded(lv_obj_t* obj, bool loaded) {
    auto* data = get_data(obj);
    if (!data || data->filament_loaded == loaded)
        return;
    data->filament_loaded = loaded;
    lv_obj_invalidate(obj);
}

void ui_system_path_canvas_set_status_text(lv_obj_t* obj, const char* text) {
    auto* data = get_data(obj);
    if (!data)
        return;
    const char* new_text = text ? text : "";
    if (strcmp(data->status_text, new_text) == 0)
        return;
    snprintf(data->status_text, sizeof(data->status_text), "%s", new_text);
    lv_obj_invalidate(obj);
}

void ui_system_path_canvas_set_bypass(lv_obj_t* obj, bool has_bypass, bool bypass_active,
                                      uint32_t bypass_color) {
    auto* data = get_data(obj);
    if (!data)
        return;
    if (data->has_bypass == has_bypass && data->bypass_active == bypass_active &&
        data->bypass_color == bypass_color)
        return;
    data->has_bypass = has_bypass;
    data->bypass_active = bypass_active;
    data->bypass_color = bypass_color;
    lv_obj_invalidate(obj);
}

void ui_system_path_canvas_set_unit_hub_sensor(lv_obj_t* obj, int unit_index, bool has_sensor,
                                               bool triggered) {
    auto* data = get_data(obj);
    if (!data || unit_index < 0 || unit_index >= SystemPathData::MAX_UNITS)
        return;
    if (data->unit_has_hub_sensor[unit_index] == has_sensor &&
        data->unit_hub_triggered[unit_index] == triggered)
        return;
    data->unit_has_hub_sensor[unit_index] = has_sensor;
    data->unit_hub_triggered[unit_index] = triggered;
    lv_obj_invalidate(obj);
}

void ui_system_path_canvas_set_toolhead_sensor(lv_obj_t* obj, bool has_toolhead_sensor,
                                               bool toolhead_sensor_triggered) {
    auto* data = get_data(obj);
    if (!data)
        return;
    if (data->has_toolhead_sensor == has_toolhead_sensor &&
        data->toolhead_sensor_triggered == toolhead_sensor_triggered)
        return;
    data->has_toolhead_sensor = has_toolhead_sensor;
    data->toolhead_sensor_triggered = toolhead_sensor_triggered;
    lv_obj_invalidate(obj);
}

void ui_system_path_canvas_set_unit_tools(lv_obj_t* obj, int unit_index, int tool_count,
                                          int first_tool) {
    auto* data = get_data(obj);
    if (!data || unit_index < 0 || unit_index >= SystemPathData::MAX_UNITS)
        return;
    if (data->unit_tool_count[unit_index] == tool_count &&
        data->unit_first_tool[unit_index] == first_tool)
        return;
    data->unit_tool_count[unit_index] = tool_count;
    data->unit_first_tool[unit_index] = first_tool;
    lv_obj_invalidate(obj);
}

void ui_system_path_canvas_set_unit_topology(lv_obj_t* obj, int unit_index, int topology) {
    auto* data = get_data(obj);
    if (!data || unit_index < 0 || unit_index >= SystemPathData::MAX_UNITS)
        return;
    if (data->unit_topology[unit_index] == topology)
        return;
    data->unit_topology[unit_index] = topology;
    lv_obj_invalidate(obj);
}

void ui_system_path_canvas_set_total_tools(lv_obj_t* obj, int total_tools) {
    auto* data = get_data(obj);
    if (!data)
        return;
    int clamped = LV_CLAMP(total_tools, 0, SystemPathData::MAX_TOOLS);
    if (data->total_tools == clamped)
        return;
    data->total_tools = clamped;
    if (!data->has_virtual_numbers) {
        for (int i = 0; i < data->total_tools; ++i) {
            snprintf(data->tool_labels[i], sizeof(data->tool_labels[i]), "T%d", i);
        }
    }
    lv_obj_invalidate(obj);
}

void ui_system_path_canvas_set_active_tool(lv_obj_t* obj, int tool_index) {
    auto* data = get_data(obj);
    if (!data || data->active_tool == tool_index)
        return;
    data->active_tool = tool_index;
    lv_obj_invalidate(obj);
}

void ui_system_path_canvas_set_current_tool(lv_obj_t* obj, int tool_index) {
    auto* data = get_data(obj);
    if (!data || data->current_tool == tool_index)
        return;
    data->current_tool = tool_index;
    if (tool_index >= 0) {
        snprintf(data->current_tool_label, sizeof(data->current_tool_label), "T%d", tool_index);
    } else {
        data->current_tool_label[0] = '\0';
    }
    lv_obj_invalidate(obj);
}

void ui_system_path_canvas_set_tool_virtual_numbers(lv_obj_t* obj, const int* numbers, int count) {
    auto* data = get_data(obj);
    if (!data)
        return;
    int n = LV_MIN(count, SystemPathData::MAX_TOOLS);
    bool changed = (data->has_virtual_numbers != (n > 0));
    if (!changed) {
        for (int i = 0; i < n && !changed; ++i) {
            if (data->tool_virtual_number[i] != numbers[i])
                changed = true;
        }
    }
    if (!changed)
        return;
    for (int i = 0; i < n; ++i) {
        data->tool_virtual_number[i] = numbers[i];
        snprintf(data->tool_labels[i], sizeof(data->tool_labels[i]), "T%d", numbers[i]);
    }
    // Clear remaining entries
    for (int i = n; i < SystemPathData::MAX_TOOLS; ++i) {
        data->tool_virtual_number[i] = i;
        snprintf(data->tool_labels[i], sizeof(data->tool_labels[i]), "T%d", i);
    }
    data->has_virtual_numbers = (n > 0);
    lv_obj_invalidate(obj);
}

void ui_system_path_canvas_set_bypass_has_spool(lv_obj_t* obj, bool has_spool) {
    auto* data = get_data(obj);
    if (data && data->bypass_has_spool != has_spool) {
        data->bypass_has_spool = has_spool;
        lv_obj_invalidate(obj);
    }
}

void ui_system_path_canvas_set_bypass_callback(lv_obj_t* obj, system_path_bypass_cb_t cb,
                                               void* user_data) {
    auto* data = get_data(obj);
    if (data) {
        data->bypass_callback = cb;
        data->bypass_user_data = user_data;
    }
}

void ui_system_path_canvas_refresh(lv_obj_t* obj) {
    lv_obj_invalidate(obj);
}
