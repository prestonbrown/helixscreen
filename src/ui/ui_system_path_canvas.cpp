// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_system_path_canvas.h"

#include "ui_fonts.h"
#include "ui_spool_drawing.h"

#include "filament_path_geometry.h"
#include "filament_tube_stroker.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl/lvgl.h"
#include "nozzle_renderer_a4t.h"
#include "nozzle_renderer_anthead.h"
#include "nozzle_renderer_bambu.h"
#include "nozzle_renderer_creality_k1.h"
#include "nozzle_renderer_creality_k2.h"
#include "nozzle_renderer_jabberwocky.h"
#include "nozzle_renderer_stealthburner.h"
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
    // X centre of each unit's card, relative to this canvas's left edge. Pushed
    // by the panel and re-pushed whenever the card row scrolls, so a stem stays
    // under the card it belongs to. Clamped at draw time by unit_stem_x().
    int32_t unit_x_positions[MAX_UNITS] = {};
    int active_unit = -1;             // -1 = none active
    uint32_t active_color = 0x4488FF; // Filament color of active path
    bool filament_loaded = false;     // Whether filament reaches nozzle
    char status_text[64] = {};        // Status label drawn to left of nozzle

    // Bypass support
    bool has_bypass = false;          // Whether to show bypass path
    bool bypass_active = false;       // Whether bypass is the active path (current_slot == -2)
    uint32_t bypass_color = 0x888888; // Color when bypass active

    // Bypass spool state (for spool box rendering)
    bool bypass_has_spool = false;

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
    // Letter in front of every toolhead badge number. 'T' = AFC lane alias (the
    // legacy default), 'E' = Klipper extruder identity. See #1229: the two
    // numbering systems disagree, so the letter says which one is on screen.
    char tool_label_prefix = 'T';
    char tool_labels[MAX_TOOLS][8] = {}; // Pre-formatted "<P>n" strings for deferred draw
    char current_tool_label[8] = {};     // Pre-formatted label for single-nozzle mode

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

// Bypass geometry shared by the draw callback and the public position getter.
// Single source of truth so the panel-side widget overlay stays anchored to
// the drawn merge point.
struct BypassGeometry {
    int32_t bypass_x;
    int32_t merge_y;
    int32_t center_x; // hub center (already shifted left when bypass is present)
};

// Mirrors ui_filament_path_canvas's BYPASS_X_RATIO so both canvases place the
// bypass spool at the same horizontal fraction — keeps a long visible tube
// segment instead of cramming the spool right next to the hub.
static constexpr float BYPASS_X_RATIO = 0.85f;

// Half-extent of the panel's BypassSpoolWidgets overlay (box is 48 + 4*2 = 56px
// wide; mirror it here so the canvas can guarantee the spool clears the
// rightmost toolhead). Kept in lockstep with BOX_SIZE in ui_bypass_spool_widget.cpp.
static constexpr int32_t BYPASS_SPOOL_HALF = 28;

static int32_t calc_tool_x(int tool_index, int total_tools, int32_t x_off, int32_t width);

static BypassGeometry compute_bypass_geometry(const SystemPathData* data,
                                              const lv_area_t& obj_coords) {
    int32_t width = lv_area_get_width(&obj_coords);
    int32_t height = lv_area_get_height(&obj_coords);
    int32_t x_off = obj_coords.x1;
    int32_t y_off = obj_coords.y1;

    // Hub shifts ~10% left to keep its label clear of the long horizontal
    // bypass merge line (single-tool, has_bypass).
    int32_t center_x = x_off + width / 2 - width / 10;

    int32_t hub_y = y_off + (int32_t)(height * HUB_Y_RATIO);
    int32_t hub_h = (int32_t)(height * HUB_HEIGHT_RATIO);
    int32_t nozzle_y = y_off + (int32_t)(height * NOZZLE_Y_RATIO);
    int32_t merge_y = (hub_y + hub_h / 2) + (nozzle_y - (hub_y + hub_h / 2)) / 3;

    // Comfortable gap between the rightmost toolhead and the bypass spool.
    int32_t comfort = LV_MAX(theme_manager_get_spacing("space_md"), 8);
    // Right boundary the spool centre must stay inside (leave room for its half
    // width + a small margin from the canvas edge).
    int32_t right_limit = x_off + width - BYPASS_SPOOL_HALF - 4;

    // Default horizontal position (single-tool / no toolhead row): the historic
    // 0.85 ratio gives a long visible tube run from the hub.
    int32_t bypass_x = x_off + (int32_t)(width * BYPASS_X_RATIO);

    // Collision-aware placement when a toolhead row is present. The rightmost
    // toolhead sits at calc_tool_x(total_tools-1, …); a toolhead glyph is about
    // a nozzle-scale wide on each side. Place the bypass spool just past that
    // glyph's right edge so the two never overlap.
    if (data->total_tools > 0) {
        int32_t rightmost_tool_x =
            calc_tool_x(data->total_tools - 1, data->total_tools, x_off, width);
        // Nozzle glyph half-width: the small (multi-tool) scale is ~3/4 of the
        // base extruder scale; a glyph spans roughly 2*scale to each side.
        int32_t scale = LV_MAX(6, data->extruder_scale * 3 / 4);
        int32_t tool_half = scale * 2;
        int32_t tool_right_edge = rightmost_tool_x + tool_half;

        int32_t want_x = tool_right_edge + comfort + BYPASS_SPOOL_HALF;
        if (want_x > bypass_x) {
            bypass_x = want_x;
        }

        if (bypass_x > right_limit) {
            // No horizontal room even at the canvas edge — drop the spool below
            // the toolhead row, centred over the rightmost toolhead, so it never
            // overlaps a nozzle. Degrades better than a clipped/overlapping spool.
            bypass_x = LV_CLAMP(rightmost_tool_x, x_off + BYPASS_SPOOL_HALF + 4, right_limit);
            // Sit below the toolhead row: nozzle centre + glyph drop + spool half.
            // Clamp so the spool's lower edge never falls off the canvas bottom on
            // short panels (the spool overlay is centred on merge_y).
            merge_y = nozzle_y + scale * 4 + BYPASS_SPOOL_HALF + comfort;
            merge_y = LV_MIN(merge_y, y_off + height - BYPASS_SPOOL_HALF - 4);
        }
    }

    bypass_x = LV_MIN(bypass_x, right_limit);

    return {bypass_x, merge_y, center_x};
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

// Tube rendering now routes through the shared concentric stroker
// (filament_tube_stroker.h), so the overview's curves are the same clean
// quarter-arc fillets as the detail panel. Overview lanes are intentionally
// thinner/dimmer than the detail panel — those width/color choices are made by
// the callers and passed straight through here via LaneStyle.
namespace pg = helix::ui::pathgeo;

// Build a solid-tube LaneStyle for the overview. The overview never draws a
// hollow PTFE bore — idle lanes are simply dimmer solid tubes — so `solid` is
// always true. `active` lanes get the wide glow backdrop (matches the detail
// panel's highlighted-path treatment).
static helix::ui::LaneStyle sp_lane_style(lv_color_t color, int32_t width, bool active) {
    helix::ui::LaneStyle st{};
    st.solid = true;
    st.color = color;
    st.bg = color; // unused for solid tubes
    st.width = width;
    st.glow = active;
    return st;
}

// Straight tube between two arbitrary points.
static void draw_tube_line(lv_layer_t* layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                           lv_color_t color, int32_t width, bool active = false) {
    pg::FilamentPath path;
    path.add_line((float)x1, (float)y1, (float)x2, (float)y2);
    helix::ui::draw_lane(layer, path, sp_lane_style(color, width, active));
}

// Vertical tube run. The concentric stroker joins segments tangentially with
// butt caps, so straight↔curve junction seams are already seamless.
static void draw_vertical_line(lv_layer_t* layer, int32_t x, int32_t y1, int32_t y2,
                               lv_color_t color, int32_t width, bool active = false) {
    helix::ui::draw_lane_vline(layer, x, y1, y2, sp_lane_style(color, width, active));
}

// Horizontal tube run (bypass merge line).
static void draw_line(lv_layer_t* layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                      lv_color_t color, int32_t width, bool active = false) {
    draw_tube_line(layer, x1, y1, x2, y2, color, width, active);
}

// Parallel cable-harness routed tube: vertical drop -> bend at horiz_y ->
// gently-descending diagonal across to ex -> drop into (ex,ey). The caller
// places horiz_y at a per-route staggered height; parallel levels are already
// well separated by their stagger, so a shallow fixed ~10% slope reads as nested
// harness routing without coinciding. 3 lines + 2 fillet arcs.
//
// Hub MERGES (single-tool convergence, multi-tool mini-hubs) do NOT use this —
// they route through helix::ui::draw_merge_fan (parallel diagonals per side,
// separation by construction).
static void draw_routed_tube_parallel(lv_layer_t* layer, int32_t sx, int32_t sy, int32_t ex,
                                      int32_t ey, int32_t horiz_y, lv_color_t color, int32_t width,
                                      bool active) {
    constexpr float FILLET_R = 9.0f;

    // Clamp the bend so it leaves room for both fillets.
    int32_t lo = sy + 4;
    int32_t hi = ey - 6;
    if (hi < lo) {
        helix::ui::draw_lane_route(layer, sx, sy, ex, ey, helix::ui::FILLET_RADIUS,
                                   sp_lane_style(color, width, active));
        return;
    }
    horiz_y = LV_CLAMP(horiz_y, lo, hi);

    int32_t dx = (ex > sx) ? (ex - sx) : (sx - ex);
    int32_t y_approach = horiz_y + dx / 10; // ~10% downward slope along the run
    if (y_approach > ey - 6)
        y_approach = ey - 6;
    if (y_approach < horiz_y)
        y_approach = horiz_y;

    pg::PathPoint pts[4] = {{(float)sx, (float)sy},
                            {(float)sx, (float)horiz_y},
                            {(float)ex, (float)y_approach},
                            {(float)ex, (float)ey}};
    pg::FilamentPath path;
    pg::route_polyline_filleted(path, pts, 4, FILLET_R);
    helix::ui::draw_lane(layer, path, sp_lane_style(color, width, active));
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
// Draw phases
// ============================================================================
// system_path_draw_cb derives one SysLayout per draw, then dispatches to the
// multi-tool or single-tool pipeline.
//
// Multi-tool (toolchanger / mixed overview — one nozzle per tool):
//   collect_routes_and_draw_unit_stems → sort_routes → draw_routes →
//   draw_mini_hubs → draw_tool_row → draw_status_centered
//
// Single-tool (all units converge on one nozzle):
//   draw_unit_columns (incl. shared merge fan) → draw_bypass_merge_line →
//   draw_combiner_hub → draw_output_to_nozzle → draw_status_beside_nozzle

// Helper: horizontal X position of a unit's entry stem.
//
// The stems are distributed evenly across the canvas, NOT tracked to the unit
// cards above. The card row is an independently scrollable container: an anchor
// sampled from a card's coordinates goes stale the instant the row scrolls, and
// once the row overflows (5 units on a 800x480) some cards sit outside the
// canvas entirely, so their stems were drawn off-canvas and survived only as
// clipped horizontal stubs running off the edge. Even distribution is stable
// under scroll, always on screen for every unit, and at 2-3 units lands within a
// few pixels of where the cards sit anyway. draw_unit_stem_labels() carries the
// stem-to-card identity that the positional tie used to imply.
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

// One dispatch point for the user's configured toolhead style.
static void draw_toolhead_glyph(lv_layer_t* layer, int32_t cx, int32_t cy, lv_color_t color,
                                int32_t scale) {
    switch (helix::SettingsManager::instance().get_effective_toolhead_style()) {
    case helix::ToolheadStyle::A4T:
        draw_nozzle_a4t(layer, cx, cy, color, scale);
        break;
    case helix::ToolheadStyle::ANTHEAD:
        draw_nozzle_anthead(layer, cx, cy, color, scale);
        break;
    case helix::ToolheadStyle::JABBERWOCKY:
        draw_nozzle_jabberwocky(layer, cx, cy, color, scale);
        break;
    case helix::ToolheadStyle::STEALTHBURNER:
        draw_nozzle_stealthburner(layer, cx, cy, color, scale);
        break;
    case helix::ToolheadStyle::CREALITY_K1:
        draw_nozzle_creality_k1(layer, cx, cy, color, scale);
        break;
    case helix::ToolheadStyle::CREALITY_K2:
        draw_nozzle_creality_k2(layer, cx, cy, color, scale);
        break;
    default:
        draw_nozzle_bambu(layer, cx, cy, color, scale);
        break;
    }
}

// Per-draw layout + resolved colors/sizes shared by every phase.
struct SysLayout {
    lv_area_t obj_coords{};
    int32_t width = 0, height = 0, x_off = 0, y_off = 0;
    int32_t entry_y = 0, merge_y = 0, hub_y = 0, hub_h = 0, tools_y = 0, nozzle_y = 0;
    int32_t center_x = 0; // shifted ~10% left in single-tool bypass layouts
    lv_color_t idle_color, active_color_lv, hub_bg, hub_border, nozzle_color;
    int32_t line_idle = 0, line_active = 0, sensor_r = 0;
    bool multi_tool = false;
};

static SysLayout compute_sys_layout(SystemPathData* data, const lv_area_t& obj_coords) {
    SysLayout L{};
    L.obj_coords = obj_coords;
    L.width = lv_area_get_width(&obj_coords);
    L.height = lv_area_get_height(&obj_coords);
    L.x_off = obj_coords.x1;
    L.y_off = obj_coords.y1;

    // Determine if multi-tool routing is needed
    L.multi_tool = (data->total_tools > 1);

    // Calculate Y positions
    L.entry_y = L.y_off + (int32_t)(L.height * ENTRY_Y_RATIO);
    L.merge_y = L.y_off + (int32_t)(L.height * MERGE_Y_RATIO);
    L.hub_y = L.y_off + (int32_t)(L.height * HUB_Y_RATIO);
    L.hub_h = (int32_t)(L.height * HUB_HEIGHT_RATIO);
    L.tools_y = L.y_off + (int32_t)(L.height * TOOLS_Y_RATIO);
    L.nozzle_y = L.y_off + (int32_t)(L.height * NOZZLE_Y_RATIO);
    L.center_x = L.x_off + L.width / 2;

    // Colors
    L.idle_color = data->color_idle;
    L.active_color_lv = lv_color_hex(data->active_color);
    L.hub_bg = data->color_hub_bg;
    L.hub_border = data->color_hub_border;
    L.nozzle_color = data->color_nozzle;

    // Sizes
    L.line_idle = data->line_width_idle;
    // Active (loaded) tubes read the SAME gauge as idle tubes — the "loaded"
    // emphasis is carried by color + glow backdrop, not extra width (matches the
    // detail panel where solid lanes dropped their +2 outline). Sensor dots keep
    // sizing off the theme's active width so they stay legible.
    L.line_active = L.line_idle;
    L.sensor_r = LV_MAX(5, data->line_width_active);
    data->cached_sensor_r = L.sensor_r;

    // Shift center_x left when bypass is supported to make room for bypass path on the right
    if (data->has_bypass && !L.multi_tool) {
        L.center_x -= L.width / 10; // Shift hub/toolhead ~10% left
    }
    return L;
}

// Horizontal position of unit `i`'s entry stem: the centre of its unit card,
// clamped into the canvas.
//
// The anchor is pushed by the panel and re-pushed on every card-row scroll -
// unit_cards_row is an independently scrollable container, and a stale anchor
// left every stem pointing at where its card used to be. The clamp covers the
// rest: once the row overflows, a scrolled-off card's centre lands outside the
// canvas, and an unclamped stem was drawn off-canvas where LVGL kept only a
// clipped horizontal stub running off the edge. Clamped, the stem parks at the
// edge it went out of, which reads as "this unit is off to that side".
static int32_t unit_stem_x(const SystemPathData* data, const SysLayout& L, int i) {
    if (i < 0 || i >= SystemPathData::MAX_UNITS) {
        return L.x_off + L.width / 2;
    }
    int32_t margin = LV_MIN(8, L.width / 4);
    return LV_CLAMP(L.x_off + data->unit_x_positions[i], L.x_off + margin,
                    L.x_off + L.width - margin);
}

// ----------------------------------------------------------------------------
// Multi-tool pipeline (per-unit routing to individual tool positions).
// Note: Bypass rendering is intentionally omitted in this mode — bypass is not
// applicable to multi-extruder toolchanger setups since each tool has its own
// filament path.
// ----------------------------------------------------------------------------

// One unit→tool route collected in the first pass, drawn in the third.
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

// Per-unit mini-hub info, deferred so hub boxes draw on top of the routes.
struct HubInfo {
    int32_t hub_x;  // centre of the hub box
    int32_t tool_x; // nozzle the hub feeds (== hub_x when nothing is shared)
    int32_t mini_hub_y;
    int32_t mini_hub_w;
    int32_t mini_hub_h;
    lv_color_t hub_bg_color;
    int first_tool;
    bool valid;
};

// Where unit `unit_index` sits among the HUB units that feed the SAME physical
// nozzle, and how many there are.
//
// compute_system_tool_layout() deliberately merges two HUB units onto one
// physical tool when they name the same extruder (a Box Turtle and a Claymore
// both wired to e0). Each unit still owns a real, separate hub though, so
// drawing both boxes at the shared nozzle's X stacked them pixel-for-pixel:
// four hub units rendered as two visible "Hub" badges, and their routes landed
// on the identical point. Fanning the boxes out around the nozzle keeps every
// hub visible and its route distinguishable.
//
// Returns rank 0 / count 1 for the overwhelmingly common unshared case, which
// puts the box exactly where it has always been.
static void hub_group_position(const SystemPathData* data, int unit_index, int* rank, int* count) {
    *rank = 0;
    *count = 0;
    const int my_tool = data->unit_first_tool[unit_index];
    for (int u = 0; u < data->unit_count && u < SystemPathData::MAX_UNITS; ++u) {
        // PARALLEL (2) and MIXED (3) do not place a hub box on a nozzle.
        if (data->unit_topology[u] == 2 || data->unit_topology[u] == 3)
            continue;
        if (data->unit_tool_count[u] <= 0 || data->unit_first_tool[u] != my_tool)
            continue;
        if (u < unit_index)
            (*rank)++;
        (*count)++;
    }
}

// PASS 1a: PARALLEL / MIXED unit — one route per unique tool position. For
// MIXED, tool_count already reflects unique nozzles (not lanes), so hub lanes
// sharing a mapped_tool produce a single route; the hub group's mini-hub is
// recorded for the last tool.
static int collect_parallel_mixed_routes(SystemPathData* data, const SysLayout& L, int i,
                                         GlobalRoute* all_routes, int total_routes,
                                         HubInfo* hub_infos) {
    int32_t unit_x = unit_stem_x(data, L, i);
    int topology = data->unit_topology[i];
    int tool_count = data->unit_tool_count[i];
    int first_tool = data->unit_first_tool[i];
    bool is_active = (i == data->active_unit);

    // Fan the lane start points apart just enough that their verticals do not
    // overlap, and no further: a wide fan detached each lane from the unit it
    // belongs to, so a toolchanger's lanes read as lines starting out of thin air
    // next to whatever hub box happened to sit there. Keep the whole fan inside
    // the unit's own column, under its label.
    int32_t column_w = (data->unit_count > 1) ? (L.width / data->unit_count) : L.width;
    int32_t spread = 0;
    if (tool_count > 1) {
        spread = LV_MIN(column_w - 8, (tool_count - 1) * LV_MAX(8, L.line_idle * 3));
        spread = LV_MAX(spread, 0);
    }
    for (int t = 0; t < tool_count && (first_tool + t) < data->total_tools; ++t) {
        int tool_idx = first_tool + t;
        int32_t tool_x = calc_tool_x(tool_idx, data->total_tools, L.x_off, L.width);
        int32_t start_x = unit_x;
        if (tool_count > 1) {
            start_x = unit_x - spread / 2 + (spread * t) / (tool_count - 1);
        }
        int32_t dist = start_x > tool_x ? (start_x - tool_x) : (tool_x - start_x);
        all_routes[total_routes++] = {i,      tool_idx,  start_x, L.entry_y,
                                      tool_x, L.tools_y, dist,    false};
    }

    // For MIXED topology, save hub info for the last tool (hub group)
    if (topology == 3 && tool_count > 1) {
        int hub_tool_idx = first_tool + tool_count - 1;
        int hub_t = tool_count - 1;
        // Use the same start_x math as the route above
        int32_t hub_start_x = unit_x;
        if (tool_count > 1) {
            hub_start_x = unit_x - spread / 2 + (spread * hub_t) / (tool_count - 1);
        }
        int32_t mhw = data->hub_width * 2 / 5;
        int32_t mhh = L.hub_h * 2 / 3;
        int32_t mhy = L.entry_y + mhh / 2 + 4;
        bool hub_has_filament =
            is_active && data->filament_loaded && (data->active_tool == hub_tool_idx);
        lv_color_t mini_bg = L.hub_bg;
        if (hub_has_filament) {
            mini_bg = sp_blend(L.hub_bg, L.active_color_lv, 0.33f);
        }
        // MIXED hubs sit on the unit's own stem, not on a nozzle, so hub_x and
        // tool_x are the same point and draw_mini_hubs() skips the outlet line.
        hub_infos[i] = {hub_start_x, hub_start_x, mhy, mhw, mhh, mini_bg, hub_tool_idx, true};
    }
    return total_routes;
}

// PASS 1b: HUB unit — one route from the unit to its mini-hub position. Also
// draws the unit's entry stem (vertical from the unit card down to the hub
// merge point, with the optional hub sensor dot interrupting it).
static int collect_hub_route_and_draw_stem(lv_layer_t* layer, SystemPathData* data,
                                           const SysLayout& L, int i, GlobalRoute* all_routes,
                                           int total_routes, HubInfo* hub_infos) {
    int32_t unit_x = unit_stem_x(data, L, i);
    int tool_count = data->unit_tool_count[i];
    int first_tool = data->unit_first_tool[i];
    bool is_active = (i == data->active_unit);

    if (tool_count <= 0 || first_tool >= data->total_tools)
        return total_routes;

    int32_t tool_x = calc_tool_x(first_tool, data->total_tools, L.x_off, L.width);
    int32_t mini_hub_w = data->hub_width * 2 / 3;
    int32_t mini_hub_h = L.hub_h * 2 / 3;
    int32_t mini_hub_y = L.merge_y + (L.tools_y - L.merge_y) / 3;
    int32_t end_y_mh = mini_hub_y - mini_hub_h / 2;

    // Fan the box away from the nozzle centre when another HUB unit feeds the
    // same one. rank 0 / count 1 (nothing shared) leaves hub_x == tool_x.
    int hub_rank = 0;
    int hub_group = 1;
    hub_group_position(data, i, &hub_rank, &hub_group);
    int32_t hub_pitch = mini_hub_w + LV_MAX(4, L.line_idle * 2);
    int32_t hub_x = tool_x + (2 * hub_rank - (hub_group - 1)) * hub_pitch / 2;

    // Hub sensor dot and short vertical beneath it
    // Use a shorter merge point for HUB units to leave more room
    // between hub routes and parallel routes below
    int32_t hub_merge_y = L.entry_y + (L.merge_y - L.entry_y) * 2 / 3;
    bool has_sensor = data->unit_has_hub_sensor[i];
    lv_color_t line_color = is_active ? L.active_color_lv : L.idle_color;
    int32_t line_w = is_active ? L.line_active : L.line_idle;
    int32_t sensor_dot_y = L.entry_y + (hub_merge_y - L.entry_y) / 3;

    if (has_sensor) {
        draw_vertical_line(layer, unit_x, L.entry_y, sensor_dot_y - L.sensor_r, line_color, line_w,
                           /*active=*/is_active);
        draw_vertical_line(layer, unit_x, sensor_dot_y + L.sensor_r, hub_merge_y, line_color,
                           line_w, /*active=*/is_active);
        bool filled = data->unit_hub_triggered[i];
        lv_color_t dot_color =
            filled ? (is_active ? L.active_color_lv : L.idle_color) : L.idle_color;
        draw_sensor_dot(layer, unit_x, sensor_dot_y, dot_color, filled, L.sensor_r);
    } else {
        draw_vertical_line(layer, unit_x, L.entry_y, hub_merge_y, line_color, line_w,
                           /*active=*/is_active);
    }

    // The route lands on this unit's own hub box, not on the nozzle - with a
    // shared nozzle those are different X positions.
    int32_t dist = unit_x > hub_x ? (unit_x - hub_x) : (hub_x - unit_x);
    all_routes[total_routes++] = {i, first_tool, unit_x, hub_merge_y, hub_x, end_y_mh, dist, true};

    // Save hub info for deferred drawing
    bool hub_has_filament = is_active && data->filament_loaded;
    lv_color_t mini_hub_bg = L.hub_bg;
    if (hub_has_filament) {
        mini_hub_bg = sp_blend(L.hub_bg, L.active_color_lv, 0.33f);
    }
    hub_infos[i] = {hub_x,      tool_x,      mini_hub_y, mini_hub_w,
                    mini_hub_h, mini_hub_bg, first_tool, true};
    return total_routes;
}

// PASS 1: Collect all routed paths across all units globally (HUB units also
// draw their entry stems). Returns the route count.
static int collect_routes_and_draw_unit_stems(lv_layer_t* layer, SystemPathData* data,
                                              const SysLayout& L, GlobalRoute* all_routes,
                                              HubInfo* hub_infos) {
    int total_routes = 0;
    for (int i = 0; i < data->unit_count && i < SystemPathData::MAX_UNITS; i++) {
        int topology = data->unit_topology[i];
        if (topology == 2 || topology == 3) {
            total_routes =
                collect_parallel_mixed_routes(data, L, i, all_routes, total_routes, hub_infos);
        } else {
            total_routes = collect_hub_route_and_draw_stem(layer, data, L, i, all_routes,
                                                           total_routes, hub_infos);
        }
    }
    return total_routes;
}

// PASS 2: Sort routes. PARALLEL by end_x ascending (leftmost tool first →
// bottom horizontal). HUB after parallel, by distance descending.
static void sort_routes(GlobalRoute* all_routes, int total_routes) {
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
}

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
static void draw_routes(lv_layer_t* layer, SystemPathData* data, const SysLayout& L,
                        GlobalRoute* all_routes, int total_routes) {
    int32_t arc_r = LV_MAX(8, (L.tools_y - L.entry_y) / 10);

    int parallel_count = 0;
    for (int r = 0; r < total_routes; ++r) {
        if (all_routes[r].start_x == all_routes[r].end_x)
            continue;
        if (!all_routes[r].is_hub)
            parallel_count++;
    }

    // PARALLEL: compute absolute Y positions for each horizontal level
    // Fixed spacing between levels (tube width * 3 gives clear visual gap)
    int32_t par_step = LV_MAX(10, L.line_idle * 3 + 4);
    // Total height of the stacked group
    int32_t par_group_h = (parallel_count > 1) ? par_step * (parallel_count - 1) : 0;
    // Center the group at 55% between entry_y and tools_y (slightly below middle)
    int32_t par_center_y = L.entry_y + (L.tools_y - L.entry_y) * 55 / 100;
    // Top of group (highest horizontal = smallest Y = last parallel route)
    int32_t par_top_y = par_center_y - par_group_h / 2;
    // Bottom of group (lowest horizontal = largest Y = first parallel route)
    int32_t par_bot_y = par_top_y + par_group_h;

    int parallel_idx = 0;

    for (int r = 0; r < total_routes; ++r) {
        auto& route = all_routes[r];
        bool is_active = (route.unit_idx == data->active_unit);
        bool tool_active = is_active && (route.tool_idx == data->active_tool);

        lv_color_t route_color = tool_active ? L.active_color_lv : L.idle_color;
        int32_t route_w = tool_active ? L.line_active : L.line_idle;

        if (route.start_x == route.end_x) {
            draw_tube_line(layer, route.start_x, route.start_y, route.end_x, route.end_y,
                           is_active ? L.active_color_lv : L.idle_color,
                           is_active ? L.line_active : L.line_idle, /*active=*/tool_active);
        } else if (route.is_hub) {
            // HUB: a single lane diagonally into its own mini-hub top, via the
            // shared merge-fan builder (n=1 lands dead-center on end_x). Each
            // unit feeds a distinct mini-hub so no two hub routes interact.
            helix::ui::MergeFanLane lane{route.start_x, route.start_y,
                                         sp_lane_style(route_color, route_w, tool_active), nullptr};
            helix::ui::draw_merge_fan(layer, &lane, 1, route.end_x, route.end_y,
                                      /*hub_w=*/0, /*fillet_r=*/9.0f);
        } else {
            // PARALLEL: idx 0 (leftmost end_x) at par_bot_y (lowest),
            // idx N-1 (rightmost end_x) at par_top_y (highest)
            int32_t horiz_y = par_bot_y - parallel_idx * par_step;
            parallel_idx++;

            horiz_y = LV_CLAMP(horiz_y, route.start_y + arc_r + 2, route.end_y - arc_r - 2);

            draw_routed_tube_parallel(layer, route.start_x, route.start_y, route.end_x, route.end_y,
                                      horiz_y, route_color, route_w,
                                      /*active=*/tool_active);
        }
    }
}

// PASS 4: Draw mini-hub boxes and hub-to-tool verticals (on top of routes).
static void draw_mini_hubs(lv_layer_t* layer, SystemPathData* data, const SysLayout& L,
                           const HubInfo* hub_infos) {
    for (int i = 0; i < data->unit_count && i < SystemPathData::MAX_UNITS; i++) {
        if (!hub_infos[i].valid)
            continue;
        const auto& hi = hub_infos[i];
        bool is_active = (i == data->active_unit);

        int topology = data->unit_topology[i];
        const char* hub_label = (topology == 3) ? "H" : "Hub";
        draw_hub_box(layer, hi.hub_x, hi.mini_hub_y, hi.mini_hub_w, hi.mini_hub_h, hi.hub_bg_color,
                     L.hub_border, data->color_text, data->label_font, data->border_radius,
                     hub_label);

        // Line from mini hub to tool (skip for MIXED — route already covers full path)
        if (topology != 3) {
            bool tool_active = is_active && (hi.first_tool == data->active_tool);
            lv_color_t out_color = tool_active ? L.active_color_lv : L.idle_color;
            int32_t out_w = tool_active ? L.line_active : L.line_idle;
            int32_t out_y = hi.mini_hub_y + hi.mini_hub_h / 2;
            if (hi.hub_x == hi.tool_x) {
                draw_vertical_line(layer, hi.tool_x, out_y, L.tools_y, out_color, out_w,
                                   /*active=*/tool_active);
            } else {
                // Shared nozzle: this hub sits beside it, so the outlet angles in.
                helix::ui::MergeFanLane lane{hi.hub_x, out_y,
                                             sp_lane_style(out_color, out_w, tool_active), nullptr};
                helix::ui::draw_merge_fan(layer, &lane, 1, hi.tool_x, L.tools_y,
                                          /*hub_w=*/0, /*fillet_r=*/9.0f);
            }
        }
    }
}

// Tool nozzles + badges along the bottom row.
static void draw_tool_row(lv_layer_t* layer, SystemPathData* data, const SysLayout& L) {
    int32_t small_scale = LV_MAX(6, data->extruder_scale * 3 / 4);
    for (int t = 0; t < data->total_tools && t < SystemPathData::MAX_TOOLS; ++t) {
        int32_t tool_x = calc_tool_x(t, data->total_tools, L.x_off, L.width);
        bool is_active_tool = (t == data->active_tool) && data->filament_loaded;

        lv_color_t noz_color = is_active_tool ? L.active_color_lv : L.nozzle_color;
        draw_toolhead_glyph(layer, tool_x, L.tools_y, noz_color, small_scale);

        // Tool badge below nozzle — use pre-formatted label from data
        if (data->label_font && t < SystemPathData::MAX_TOOLS) {
            draw_tool_badge(layer, tool_x, L.tools_y, small_scale, data->tool_labels[t],
                            data->label_font, data->color_idle,
                            is_active_tool ? L.active_color_lv : data->color_text);
        }
    }
}

// Status text centered along the bottom edge (multi-tool layout).
static void draw_status_centered(lv_layer_t* layer, SystemPathData* data, const SysLayout& L) {
    if (!data->status_text[0] || !data->label_font)
        return;
    lv_draw_label_dsc_t status_dsc;
    lv_draw_label_dsc_init(&status_dsc);
    status_dsc.color = data->color_text;
    status_dsc.font = data->label_font;
    status_dsc.align = LV_TEXT_ALIGN_CENTER;
    status_dsc.text = data->status_text;

    int32_t font_h = lv_font_get_line_height(data->label_font);
    int32_t status_y = L.y_off + L.height - font_h - 2;
    lv_area_t status_area = {L.x_off + 4, status_y, L.x_off + L.width - 4, status_y + font_h};
    lv_draw_label(layer, &status_dsc, &status_area);
}

static void draw_multi_tool(lv_layer_t* layer, SystemPathData* data, const SysLayout& L) {
    GlobalRoute all_routes[SystemPathData::MAX_TOOLS];
    HubInfo hub_infos[SystemPathData::MAX_UNITS] = {};

    int total_routes = collect_routes_and_draw_unit_stems(layer, data, L, all_routes, hub_infos);
    sort_routes(all_routes, total_routes);
    draw_routes(layer, data, L, all_routes, total_routes);
    draw_mini_hubs(layer, data, L, hub_infos);
    draw_tool_row(layer, data, L);
    draw_status_centered(layer, data, L);
}

// ----------------------------------------------------------------------------
// Single-tool pipeline (hub convergence to one nozzle).
// ----------------------------------------------------------------------------

// Unit entry columns (one per unit, with optional hub sensor dot) converging
// into the combiner hub via the shared parallel-diagonal merge fan
// (separation by construction — no overlaps or pinches).
static void draw_unit_columns(lv_layer_t* layer, SystemPathData* data, const SysLayout& L) {
    int32_t end_y_hub = L.hub_y - L.hub_h / 2;
    helix::ui::MergeFanLane conv_lanes[SystemPathData::MAX_UNITS];
    int conv_n = 0;

    // Draw unit entry lines (one per unit, from entry to merge point) and
    // collect each unit's convergence lane for one shared draw_merge_fan call.
    for (int i = 0; i < data->unit_count && i < SystemPathData::MAX_UNITS; i++) {
        int32_t unit_x = unit_stem_x(data, L, i);
        bool is_active = (i == data->active_unit);

        lv_color_t line_color = is_active ? L.active_color_lv : L.idle_color;
        int32_t line_w = is_active ? L.line_active : L.line_idle;

        // Hub sensor dot interrupts the vertical segment
        bool has_sensor = data->unit_has_hub_sensor[i];
        int32_t sensor_dot_y = L.entry_y + (L.merge_y - L.entry_y) * 3 / 5;

        if (has_sensor) {
            draw_vertical_line(layer, unit_x, L.entry_y, sensor_dot_y - L.sensor_r, line_color,
                               line_w, /*active=*/is_active);
            draw_vertical_line(layer, unit_x, sensor_dot_y + L.sensor_r, L.merge_y, line_color,
                               line_w, /*active=*/is_active);
            bool filled = data->unit_hub_triggered[i];
            lv_color_t dot_color =
                filled ? (is_active ? L.active_color_lv : L.idle_color) : L.idle_color;
            draw_sensor_dot(layer, unit_x, sensor_dot_y, dot_color, filled, L.sensor_r);
        } else {
            draw_vertical_line(layer, unit_x, L.entry_y, L.merge_y, line_color, line_w,
                               /*active=*/is_active);
        }

        // Collect this unit's convergence lane (unit column down to the hub
        // top). The shared builder draws them all as parallel diagonals per
        // side after the loop.
        conv_lanes[conv_n++] = {unit_x, L.merge_y, sp_lane_style(line_color, line_w, is_active),
                                nullptr};
    }

    // Single shared draw: parallel-diagonal merge fan into the combiner hub.
    helix::ui::draw_merge_fan(layer, conv_lanes, conv_n, L.center_x, end_y_hub, data->hub_width,
                              /*fillet_r=*/9.0f);
}

// Bypass merge line. Spool + labels are rendered by the panel via the shared
// BypassSpoolWidgets overlay centered on the merge point — same model as
// ui_filament_path_canvas, so both AMS panels present the bypass identically.
static void draw_bypass_merge_line(lv_layer_t* layer, SystemPathData* data, const SysLayout& L) {
    if (!data->has_bypass)
        return;
    BypassGeometry bg = compute_bypass_geometry(data, L.obj_coords);
    bool bp_active = data->bypass_active;
    lv_color_t bp_color = bp_active ? lv_color_hex(data->bypass_color) : L.idle_color;
    int32_t bp_width = bp_active ? L.line_active : L.line_idle;

    // Horizontal line from spool/merge to hub (line ends inside the
    // spool widget; the widget is opaque so the overlap isn't visible).
    draw_line(layer, bg.bypass_x, bg.merge_y, bg.center_x + L.sensor_r, bg.merge_y, bp_color,
              bp_width, /*active=*/bp_active);
    draw_sensor_dot(layer, bg.center_x, bg.merge_y, bp_color, bp_active, L.sensor_r);
}

// The combiner hub box, tinted when filament is loaded through it.
static void draw_combiner_hub(lv_layer_t* layer, SystemPathData* data, const SysLayout& L) {
    bool hub_has_filament = (data->active_unit >= 0 && data->filament_loaded);
    lv_color_t hub_bg_tinted = L.hub_bg;
    if (hub_has_filament) {
        hub_bg_tinted = sp_blend(L.hub_bg, L.active_color_lv, 0.33f);
    }
    draw_hub_box(layer, L.center_x, L.hub_y, data->hub_width, L.hub_h, hub_bg_tinted, L.hub_border,
                 data->color_text, data->label_font, data->border_radius, "Hub");
}

// Output line from hub to nozzle: sensor dots, the nozzle glyph, and the
// virtual tool badge.
static void draw_output_to_nozzle(lv_layer_t* layer, SystemPathData* data, const SysLayout& L) {
    bool unit_active = (data->active_unit >= 0 && data->filament_loaded);
    bool bp_active = (data->bypass_active && data->filament_loaded);
    bool any_active = unit_active || bp_active;

    int32_t hub_bottom = L.hub_y + L.hub_h / 2;
    int32_t extruder_half_height = data->extruder_scale * 2;
    int32_t nozzle_top = L.nozzle_y - extruder_half_height;
    int32_t bypass_merge_y = hub_bottom + (L.nozzle_y - hub_bottom) / 3;
    int32_t toolhead_sensor_y = hub_bottom + (nozzle_top - hub_bottom) * 2 / 3;

    lv_color_t active_output_color =
        bp_active ? lv_color_hex(data->bypass_color) : L.active_color_lv;

    if (bp_active) {
        draw_vertical_line(layer, L.center_x, hub_bottom, bypass_merge_y, L.idle_color,
                           L.line_idle);
        draw_vertical_line(layer, L.center_x, bypass_merge_y, nozzle_top,
                           lv_color_hex(data->bypass_color), L.line_active, /*active=*/true);
    } else if (unit_active) {
        draw_vertical_line(layer, L.center_x, hub_bottom, nozzle_top, L.active_color_lv,
                           L.line_active, /*active=*/true);
    } else {
        draw_vertical_line(layer, L.center_x, hub_bottom, nozzle_top, L.idle_color, L.line_idle);
    }

    if (data->has_toolhead_sensor) {
        bool th_filled = data->toolhead_sensor_triggered;
        lv_color_t th_dot_color = th_filled ? active_output_color : L.idle_color;
        if (!any_active)
            th_dot_color = L.idle_color;
        draw_sensor_dot(layer, L.center_x, toolhead_sensor_y, th_dot_color, th_filled, L.sensor_r);
    }

    lv_color_t noz_color = L.nozzle_color;
    if (bp_active) {
        noz_color = lv_color_hex(data->bypass_color);
    } else if (unit_active) {
        noz_color = L.active_color_lv;
    }

    draw_toolhead_glyph(layer, L.center_x, L.nozzle_y, noz_color, data->extruder_scale);

    // Virtual tool badge beneath nozzle — only when multiple slots feed one toolhead
    if (data->total_tools <= 1 && data->current_tool >= 0 && data->label_font) {
        lv_color_t badge_text = (unit_active || bp_active) ? noz_color : data->color_text;
        draw_tool_badge(layer, L.center_x, L.nozzle_y, data->extruder_scale,
                        data->current_tool_label, data->label_font, data->color_idle, badge_text);
    }
}

// Status text right-aligned beside the nozzle (single-tool layout).
static void draw_status_beside_nozzle(lv_layer_t* layer, SystemPathData* data, const SysLayout& L) {
    if (!data->status_text[0] || !data->label_font)
        return;
    lv_draw_label_dsc_t status_dsc;
    lv_draw_label_dsc_init(&status_dsc);
    status_dsc.color = data->color_text;
    status_dsc.font = data->label_font;
    status_dsc.align = LV_TEXT_ALIGN_RIGHT;
    status_dsc.text = data->status_text;

    int32_t font_h = lv_font_get_line_height(data->label_font);
    int32_t label_right = L.center_x - data->extruder_scale * 3;
    int32_t label_left = L.x_off + 4;
    lv_area_t status_area = {label_left, L.nozzle_y - font_h / 2, label_right,
                             L.nozzle_y + font_h / 2};
    lv_draw_label(layer, &status_dsc, &status_area);
}

static void draw_single_tool(lv_layer_t* layer, SystemPathData* data, const SysLayout& L) {
    draw_unit_columns(layer, data, L);
    draw_bypass_merge_line(layer, data, L);
    draw_combiner_hub(layer, data, L);
    draw_output_to_nozzle(layer, data, L);
    draw_status_beside_nozzle(layer, data, L);
}

// ============================================================================
// Main Draw Callback
// ============================================================================

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

    lv_area_t obj_coords;
    lv_obj_get_coords(obj, &obj_coords);
    SysLayout L = compute_sys_layout(data, obj_coords);

    if (L.multi_tool) {
        draw_multi_tool(layer, data, L);
    } else {
        draw_single_tool(layer, data, L);
    }

    spdlog::trace("[SystemPath] Draw: units={}, active={}, loaded={}, tools={}, active_tool={}, "
                  "current_tool={}, bypass={}(active={})",
                  data->unit_count, data->active_unit, data->filament_loaded, data->total_tools,
                  data->active_tool, data->current_tool, data->has_bypass, data->bypass_active);
}

// ============================================================================
// Event Handlers
// ============================================================================

// Bypass spool clicks are handled by the BypassSpoolWidgets overlay the panel
// places on top of this canvas — see ui_bypass_spool_widget.h.

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
    // Click handling on the canvas is no longer needed — bypass clicks are
    // captured by the BypassSpoolWidgets overlay the panel places on top.

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
    // Click handling on the canvas is no longer needed — bypass clicks are
    // captured by the BypassSpoolWidgets overlay the panel places on top.

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
            snprintf(data->tool_labels[i], sizeof(data->tool_labels[i]), "%c%d",
                     data->tool_label_prefix, i);
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
        // Single-nozzle mode keeps the AFC lane alias on purpose, and so ignores
        // tool_label_prefix. With one extruder the three numbering systems cannot
        // disagree about *which* toolhead is meant (#1229), so the alias is the
        // only informative number available — an extruder identity here would be
        // a constant "E0". Multi-nozzle badges go through tool_labels[] instead.
        snprintf(data->current_tool_label, sizeof(data->current_tool_label), "T%d", tool_index);
    } else {
        data->current_tool_label[0] = '\0';
    }
    lv_obj_invalidate(obj);
}

void ui_system_path_canvas_set_tool_label_prefix(lv_obj_t* obj, char prefix) {
    auto* data = get_data(obj);
    if (!data || prefix == '\0' || data->tool_label_prefix == prefix)
        return;
    data->tool_label_prefix = prefix;
    // Reformat in place: the numbers can be unchanged while the letter flips,
    // and set_tool_virtual_numbers() short-circuits on unchanged numbers.
    for (int i = 0; i < SystemPathData::MAX_TOOLS; ++i) {
        const int n = data->has_virtual_numbers ? data->tool_virtual_number[i] : i;
        snprintf(data->tool_labels[i], sizeof(data->tool_labels[i]), "%c%d", prefix, n);
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
    const char prefix = data->tool_label_prefix;
    for (int i = 0; i < n; ++i) {
        data->tool_virtual_number[i] = numbers[i];
        snprintf(data->tool_labels[i], sizeof(data->tool_labels[i]), "%c%d", prefix, numbers[i]);
    }
    // Clear remaining entries
    for (int i = n; i < SystemPathData::MAX_TOOLS; ++i) {
        data->tool_virtual_number[i] = i;
        snprintf(data->tool_labels[i], sizeof(data->tool_labels[i]), "%c%d", prefix, i);
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

bool ui_system_path_canvas_get_bypass_merge_pos(lv_obj_t* obj, int32_t* cx_out, int32_t* cy_out) {
    auto* data = get_data(obj);
    if (!data || !data->has_bypass) {
        return false;
    }
    // Note: this intentionally serves both single-tool AND multi-tool (toolhead
    // row) layouts. compute_bypass_geometry() places the spool clear of the
    // rightmost toolhead, so the panel-side overlay no longer collides when many
    // tools span the canvas (e.g. HTLF + toolchanger, 7 tools). In multi-tool
    // mode the draw callback omits the bypass merge tube entirely (each tool has
    // its own path), so the overlay deliberately shows a floating, tube-less
    // bypass spool — this getter still returns its anchor for that overlay.
    lv_obj_update_layout(obj);
    lv_area_t obj_coords;
    lv_obj_get_coords(obj, &obj_coords);
    if (lv_area_get_width(&obj_coords) <= 0 || lv_area_get_height(&obj_coords) <= 0) {
        return false;
    }
    BypassGeometry bg = compute_bypass_geometry(data, obj_coords);
    if (cx_out) {
        *cx_out = bg.bypass_x;
    }
    if (cy_out) {
        *cy_out = bg.merge_y;
    }
    return true;
}

void ui_system_path_canvas_refresh(lv_obj_t* obj) {
    lv_obj_invalidate(obj);
}
