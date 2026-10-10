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
    // Unit cards the overview can draw; a unit past this has no stem, no hub,
    // and a hub group made only of such units is missing from the plan.
    static constexpr int MAX_UNITS = 16;
    static constexpr int MAX_TOOLS = 16;
    // X centre of each unit's card, relative to this canvas's left edge. Pushed
    // by the panel and re-pushed whenever the card row scrolls, so a stem stays
    // under the card it belongs to. Clamped at draw time by unit_stem_x().
    int32_t unit_x_positions[MAX_UNITS] = {};
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
    bool unit_hub_triggered[MAX_UNITS] = {};  // Per-unit hub sensor state
    bool unit_has_hub_sensor[MAX_UNITS] = {}; // Per-unit hub sensor capability
    // Each unit's furthest-loaded lane: how far it reached and its color.
    PathSegment unit_lane_segment[MAX_UNITS] = {};
    uint32_t unit_lane_color[MAX_UNITS] = {};

    // Toolhead sensor state
    bool has_toolhead_sensor = false; // System has a toolhead entry sensor

    // The buffer between the hub and the nozzle (single-toolhead layout): the same
    // box the unit view draws for the system reading, so its label and severity
    // come from ams_detail_buffer_box().
    bool has_buffer = false;
    int buffer_fault = 0; // severity: -1 untinted, 0 neutral, 1 warning, 2 danger
    char buffer_label[8] = {};

    // Per-unit hub (several-toolhead layout). Units carrying the same non-zero
    // group join one hub box; 0 is a unit on a hub of its own.
    int unit_hub_group[MAX_UNITS] = {};
    // Per-unit buffer under that unit's hub, in the same terms as buffer_*.
    bool unit_has_buffer[MAX_UNITS] = {};
    int unit_buffer_fault[MAX_UNITS] = {};
    char unit_buffer_label[MAX_UNITS][8] = {};

    // Per-unit tool routing (mixed topology support)
    int unit_tool_count[MAX_UNITS] = {};     // Tools per unit (BT=4, OpenAMS=1)
    int unit_first_tool[MAX_UNITS] = {};     // First tool index for this unit
    int unit_topology[MAX_UNITS] = {};       // 0=LINEAR, 1=HUB, 2=PARALLEL, 3=MIXED
    bool unit_absent[MAX_UNITS] = {};        // Box not on the bus: column drawn empty
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
    const lv_font_t* label_font = nullptr;
};

// Vertical layout. The unit stems start at ENTRY_Y_RATIO of the height, and the
// toolhead glyphs stand on the GLYPH_BOTTOM line; everything between sits a
// fixed fraction of the way down that run.
inline constexpr float ENTRY_Y_RATIO = 0.05f;
inline constexpr float SINGLE_GLYPH_BOTTOM_RATIO = 0.93f; // the one nozzle
inline constexpr float MULTI_GLYPH_BOTTOM_RATIO = 0.80f;  // the tool row

// Per-draw layout shared by the planner and the canvas.
struct SysLayout {
    int32_t width = 0, height = 0, x_off = 0, y_off = 0;
    int32_t entry_y = 0, merge_y = 0, hub_y = 0, hub_h = 0, tools_y = 0, nozzle_y = 0;
    int32_t center_x = 0; // shifted ~10% left in single-tool bypass layouts
    bool multi_tool = false;
};

SysLayout compute_sys_layout(const SystemPathData& data, const lv_area_t& obj_coords);

// Bypass geometry shared by the planner and the public position getter, so the
// panel-side widget overlay stays anchored to the planned merge point.
struct BypassGeometry {
    int32_t bypass_x;
    int32_t merge_y;
    int32_t center_x; // hub center (already shifted left when bypass is present)
};
BypassGeometry compute_bypass_geometry(const SystemPathData& data, const lv_area_t& obj_coords);

int32_t calc_tool_x(int tool_index, int total_tools, int32_t x_off, int32_t width);
/// Scale of the toolhead glyphs in the multi-tool row.
int32_t small_tool_scale(const SystemPathData& data);

// One box the canvas draws over the tubes: a multi-tool hub. A hub shared by
// several units is recorded once, at its lowest unit index.
struct HubInfo {
    int32_t hub_x;  // centre of the hub box
    int32_t tool_x; // nozzle the hub feeds (== hub_x when nothing is shared)
    int32_t mini_hub_y;
    int32_t mini_hub_w;
    int32_t mini_hub_h;
    lv_color_t hub_bg_color;
    int first_tool;
    bool valid;
    // The hub's buffer box on its outlet; buffer_h == 0 when the hub has none.
    int32_t buffer_y = 0;
    int32_t buffer_w = 0;
    int32_t buffer_h = 0;
};

// What the canvas draws on top of the planned tubes.
struct OverviewBoxes {
    HubInfo hubs[SystemPathData::MAX_UNITS] = {};
    lv_color_t combiner_bg; // single-tool combiner hub fill
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
