// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file ui_system_path_plan.h
 * @brief The multi-unit overview canvas's state, layout and route plan.
 *
 * The overview keeps its own geometry (unit stems under the cards, the
 * combiner hub or the toolhead row, the bypass merge) and plans it into the
 * same PathPlan the detail views use: span styles and band states come from
 * span_style()/band_state(), and paint_tubes/paint_box_bands draw it.
 */

#include "ui_filament_path_plan.h"

#include "ams_types.h"
#include "settings_manager.h"

namespace helix::ui::syspath {

using fpath::PathPlan;

struct SystemPathData {
    int unit_count = 0;
    // Unit cards the overview can draw (kAmsMaxUnits, shared with AmsState); a unit past this has
    // no stem, no hub, and a hub group made only of such units is missing from the plan.
    static constexpr int kMaxUnits = helix::kAmsMaxUnits;
    static constexpr int kMaxTools = 16;
    // X center of each unit's card, relative to this canvas's left edge. Pushed
    // by the panel and re-pushed whenever the card row scrolls, so a stem stays
    // under the card it belongs to. Clamped at draw time by unit_stem_x().
    int32_t unit_x_positions[kMaxUnits] = {};
    int active_unit = -1;             // -1 = none active
    uint32_t active_color = 0x4488FF; // Filament color of active path
    bool filament_loaded = false;     // Whether filament reaches nozzle
    char status_text[64] = {};        // Status label drawn to left of nozzle

    // How far the active filament reached, and where the system reports an error.
    PathSegment filament_segment = PathSegment::NONE;
    PathSegment error_segment = PathSegment::NONE;

    // Bypass support
    bool has_bypass = false;          // Whether to show bypass path
    bool bypass_active = false;       // Whether bypass is the active path (current_slot == -2)
    uint32_t bypass_color = 0x888888; // Color when bypass active

    // Bypass spool state (for spool box rendering)
    bool bypass_has_spool = false;

    // Per-unit hub sensor states
    bool unit_hub_triggered[kMaxUnits] = {};  // Per-unit hub sensor state
    bool unit_has_hub_sensor[kMaxUnits] = {}; // Per-unit hub sensor capability
    // Each unit's furthest-loaded lane: how far it reached and its color.
    PathSegment unit_lane_segment[kMaxUnits] = {};
    uint32_t unit_lane_color[kMaxUnits] = {};

    // Toolhead sensor state
    bool has_toolhead_sensor = false; // System has a toolhead entry sensor

    // The buffer between the hub and the nozzle (single-toolhead layout): the same
    // box the unit view draws for the system reading, so its label and severity
    // come from ams_detail_buffer_box().
    bool has_buffer = false;
    int buffer_fault = 0; // severity: -1 untinted, 0 neutral, 1 warning, 2 danger
    char buffer_label[8] = {};

    // Per-unit hub (several-toolhead layout): units carrying the same non-zero
    // group are on one physical hub; 0 is a unit on a hub of its own. The plan
    // draws every route into a nozzle through one junction box, so a shared hub
    // needs no handling of its own there.
    int unit_hub_group[kMaxUnits] = {};
    // Per-unit buffer under that unit's hub, in the same terms as buffer_*.
    bool unit_has_buffer[kMaxUnits] = {};
    int unit_buffer_fault[kMaxUnits] = {};
    char unit_buffer_label[kMaxUnits][8] = {};

    // Per-unit tool routing (mixed topology support)
    int unit_tool_count[kMaxUnits] = {};     // Tools per unit (BT=4, OpenAMS=1)
    int unit_first_tool[kMaxUnits] = {};     // First tool index for this unit
    int unit_topology[kMaxUnits] = {};       // 0=LINEAR, 1=HUB, 2=PARALLEL, 3=MIXED
    bool unit_absent[kMaxUnits] = {};        // Box not on the bus: column drawn empty
    int total_tools = 0;                     // Total tool count across all units
    int active_tool = -1;                    // Currently active tool (-1=none)
    int current_tool = -1;                   // Virtual tool number (slot-based, for label)
    int tool_virtual_number[kMaxTools] = {}; // Virtual tool labels per physical nozzle
    bool has_virtual_numbers = false;        // When false, raw physical index is used for labels
    // Letter in front of every toolhead badge number. 'T' = AFC lane alias (the
    // legacy default), 'E' = Klipper extruder identity. See #1229: the two
    // numbering systems disagree, so the letter says which one is on screen.
    char tool_label_prefix = 'T';
    char tool_labels[kMaxTools][8] = {}; // Pre-formatted "<P>n" strings for deferred draw
    char current_tool_label[8] = {};     // Pre-formatted label for single-nozzle mode

    // Theme-derived colors, read on every draw
    lv_color_t color_idle;
    lv_color_t color_hub_bg;
    lv_color_t color_hub_border;
    lv_color_t color_text;
    lv_color_t color_error;
    lv_color_t color_bg;
    lv_color_t color_accent;

    ToolheadStyle toolhead_style = ToolheadStyle::DEFAULT; // effective style, read each draw

    // Theme-derived sizes
    int32_t tube_gauge = 5;
    int32_t sensor_radius = 5;
    int32_t hub_width = 80;
    int32_t hub_height = 30;
    int32_t border_radius = 6;
    int32_t extruder_scale = 10;
    int32_t space_md = 8;
    int32_t space_xs = 4;
    const lv_font_t* label_font = nullptr;
    // label_font's line height; 0 when no font is measured (every box then
    // takes its labels).
    int32_t label_line_h = 0;
};

// Vertical layout. The unit stems start at kEntryYRatio of the height, and the
// toolhead glyphs stand on the GLYPH_BOTTOM line; everything between sits a
// fixed fraction of the way down that run.
inline constexpr float kEntryYRatio = 0.05f;
inline constexpr float kSingleGlyphBottomRatio = 0.93f; // the one nozzle
inline constexpr float kMultiGlyphBottomRatio = 0.80f;  // the tool row

// Per-draw layout shared by the planner and the canvas.
struct SysLayout {
    int32_t width = 0, height = 0, x_off = 0, y_off = 0;
    int32_t entry_y = 0, merge_y = 0, hub_y = 0, hub_h = 0, tools_y = 0, nozzle_y = 0;
    int32_t center_x = 0; // shifted ~10% left in single-tool bypass layouts
    bool multi_tool = false;
    bool hub_labeled = true; // the single-toolhead combiner fits its label
};

SysLayout compute_sys_layout(const SystemPathData& data, const lv_area_t& obj_coords);

/// A labelled box's height: at least @p base_h, and tall enough for one line
/// of the label font plus space_xs when that fits in @p room. When it does not,
/// the box keeps @p base_h and @p labeled says to draw it without text.
int32_t labeled_box_h(const SystemPathData& data, int32_t base_h, int32_t room, bool* labeled);

/// The single-toolhead buffer box on the trunk: its center, height and
/// whether its label fits.
struct TrunkBuffer {
    int32_t y, h;
    bool labeled;
};
TrunkBuffer trunk_buffer(const SystemPathData& data, const SysLayout& L);

// Bypass geometry shared by the planner and the public position getter, so the
// panel-side widget overlay stays anchored to the planned merge point.
struct BypassGeometry {
    int32_t bypass_x;
    int32_t merge_y;
    int32_t center_x; // hub center (already shifted left when bypass is present)
};
BypassGeometry compute_bypass_geometry(const SystemPathData& data, const lv_area_t& obj_coords);

/// @p v clamped into [@p lo, @p hi]. A span too short to hold anything
/// (@p lo > @p hi) yields its midpoint, never a bound lying past the other.
int32_t clamp_span(int32_t lo, int32_t v, int32_t hi);

int32_t calc_tool_x(int tool_index, int total_tools, int32_t x_off, int32_t width);
/// Scale of the toolhead glyphs in the multi-tool row.
int32_t small_tool_scale(const SystemPathData& data);

// What feeds one nozzle. A hub is a combiner: it is drawn only where two or
// more routes enter and one leaves. One route into a nozzle is a unit's own
// line (the unit card stands for the merge of its lanes), so it gets no hub
// box; two or more routes, from any units and topologies, meet in ONE box
// just above the nozzle.
struct NozzleFeed {
    int routes = 0;          // routes entering the nozzle
    bool junction = false;   // routes >= 2: one hub box joins them
    bool hub_sensor = false; // a hub at the junction reads filament presence
    // A buffer on the way into the nozzle: inside the junction's hub box, or
    // inline on the one route. buffer_unit is the unit whose buffer it is
    // (unit_buffer_*), or -1 for the system reading (buffer_*).
    bool buffer = false;
    int buffer_unit = -1;
};

/// The routes into physical nozzle @p tool and what joins them. With one
/// toolhead every present unit feeds it; with several, a PARALLEL or MIXED unit
/// feeds each of its tools and any other unit feeds its first tool. The hub
/// sensor and the buffer come from the units with a hub on the way in (a HUB or
/// LINEAR unit, a MIXED unit's hub-routed tool).
NozzleFeed nozzle_feed(const SystemPathData& data, int tool);

// What the canvas draws over the tubes at one nozzle of the several-toolhead
// layout: the junction's hub box, or the buffer on a lone route.
struct HubInfo {
    int32_t hub_x;  // center of the hub box (or of the lone route's buffer)
    int32_t tool_x; // nozzle the box feeds (== hub_x unless the row moved it)
    int32_t mini_hub_y;
    int32_t mini_hub_w;
    int32_t mini_hub_h;
    lv_color_t hub_bg_color;
    bool valid; // a junction: the hub box is drawn
    // The buffer box on a lone route, between where its lanes merged and the
    // nozzle; buffer_h == 0 when there is none.
    int32_t buffer_y = 0;
    int32_t buffer_w = 0;
    int32_t buffer_h = 0;
    bool short_label = false;   // too narrow for "Hub": draw "H"
    bool labeled = true;        // the row is tall enough for a line of text
    bool buffer_in_hub = false; // the junction's hub carries the buffer: one box
    int buffer_unit = -1;       // whose unit_buffer_* the buffer shows
};

// What the canvas draws on top of the planned tubes.
struct OverviewBoxes {
    HubInfo hubs[SystemPathData::kMaxTools] = {}; // by physical tool
    // One toolhead: the combiner hub box, drawn when two or more units meet,
    // and whether it carries the system buffer.
    bool combiner = false;
    bool combiner_buffer = false;
    bool combiner_labeled = true; // the combiner is tall enough for its label
    lv_color_t combiner_bg;       // single-tool combiner hub fill
};

/// One unit's route state: the active unit carries the system's filament
/// segment; another unit is loaded as far as its hub when its hub sensor
/// reads filament, in its furthest-loaded lane's color (the wall color when
/// no lane reports one).
fpath::Lane unit_lane(const SystemPathData& data, int unit);

/// Plan every tube and sensor band of the overview into @p plan.
void plan_overview(const SystemPathData& data, const SysLayout& L, PathPlan& plan,
                   OverviewBoxes& boxes);

/// The state behind a system_path_canvas widget, or nullptr for any other object.
const SystemPathData* system_path_data(lv_obj_t* obj);

/// Walls, accent, error, background and gauge of the overview's tubes.
fpath::TubePalette overview_palette(const SystemPathData& data);

} // namespace helix::ui::syspath
