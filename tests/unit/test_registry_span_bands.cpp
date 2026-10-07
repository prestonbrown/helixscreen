// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_registry_span_bands.cpp
 * @brief Pins the pixel band every authored registry span lands in, on every
 *        shipping panel geometry.
 *
 * A home widget picks its layout from the pixels it occupies, not from its grid
 * span (`include/panel_widget_size.h`). The span is authored once and read on
 * every panel, so a span edit that looks harmless can quietly demote a widget to
 * its compact layout on one geometry and leave the other seven alone. This walks
 * the registry against all eight shipping geometries and asserts the band each
 * span produces, so that demotion is a red test rather than a visual surprise on
 * one panel.
 *
 * This is arithmetic over the registry table and the grid metrics helpers. It
 * proves the authored numbers still resolve to the intended bands; it does not
 * exercise PanelWidgetManager, so it cannot prove a widget is actually handed
 * that size at runtime — tests/unit/test_panel_widget_manager_cell_px.cpp covers
 * the wiring.
 */

#include "ui_breakpoint.h"

#include "grid_layout.h"
#include "panel_widget_registry.h"
#include "panel_widget_size.h"

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::widget_size;

namespace {

/// Measured container content boxes and gutters, one per shipping tier.
/// content_w/content_h are the home grid container's content box, which is a
/// property of the panel chrome and does not change with the track count; the
/// gutter is the space_xs token at that breakpoint. Both are what
/// GridLayout::get_dimensions() and grid_cell_metrics() divide, so a tier is
/// fully described by them plus the breakpoint its resolution selects.
///
/// Read off a live instance per tier: `HELIX_SCREEN_SIZE=<WxH> helix-screen
/// --test -vv`, then the `[PanelWidgetManager] Grid layout:` and
/// `Track geometry:` lines.
struct Geometry {
    const char* name;
    int panel_w, panel_h;
    int content_w, content_h;
    int gutter;
};

// clang-format off
const std::vector<Geometry> kGeometries = {
    //  name                       panel      content   gutter
    {"micro 480x272",           480,  272,  430, 264,  2},
    {"tiny 480x320",            480,  320,  418, 312,  2},
    {"small 480x400",           480,  400,  414, 388,  4},
    {"medium 800x480",          800,  480,  710, 466,  5},
    {"large 1024x600",         1024,  600,  904, 584,  6},
    {"xlarge 1280x720",        1280,  720, 1128, 700,  8},
    {"micro portrait 272x480",  272,  480,  264, 394,  2},
    {"portrait 480x800",        480,  800,  466, 664,  5},
};
// clang-format on

/// The tier a panel's own widgets are laid out against.
UiBreakpoint tier_of(const Geometry& g) {
    return breakpoint_for(std::min(g.panel_w, g.panel_h));
}

/// 0 = compact, 1 = normal, 2 = wide. Compared the way the widgets compare:
/// `>=` against the truncated integer pixel extent they are handed, and
/// against the bands of the panel the widget is on — the bands scale with the
/// type ladder, so a band is only meaningful next to its own tier.
int width_band(int px, UiBreakpoint bp) {
    if (px >= w_wide(bp))
        return 2;
    if (px >= w_normal(bp))
        return 1;
    return 0;
}

int height_band(int px, UiBreakpoint bp) {
    if (px >= h_taller(bp))
        return 2;
    if (px >= h_tall(bp))
        return 1;
    return 0;
}

/// Track geometry for one tier, taken from the same helpers PanelWidgetManager
/// uses so the two cannot drift.
CellMetrics metrics_for(const Geometry& g) {
    auto d = GridLayout::get_dimensions(tier_of(g), g.content_w, g.content_h);
    return grid_cell_metrics(g.content_w, g.content_h, d.cols, d.rows, g.gutter);
}

/// PanelWidgetManager truncates the float extent before handing it to
/// on_size_changed() (panel_widget_manager.cpp), so the band has to be read off
/// the truncated value or a widget sitting exactly on a threshold reads
/// differently here than it does at runtime.
int extent_px(float cell, int gutter, int span) {
    return static_cast<int>(grid_track_extent(cell, gutter, span));
}

} // namespace

TEST_CASE("registry spans: no authored span exceeds the narrowest grid",
          "[widget_def][span_bands]") {
    // A widget wider or taller at its declared MINIMUM than the grid is
    // TooLargeForGrid and gets disabled at boot with a toast. 272x480 is the
    // narrowest column axis of any shipping panel.
    auto d = GridLayout::get_dimensions(UiBreakpoint::Micro, 264, 394);
    REQUIRE(d.cols == 8);
    REQUIRE(d.rows == 12);

    for (const auto& def : get_all_widget_defs()) {
        INFO("widget " << def.id << " min " << def.effective_min_colspan() << "x"
                       << def.effective_min_rowspan() << " on a " << d.cols << "x" << d.rows
                       << " grid");
        CHECK(def.effective_min_colspan() <= d.cols);
        CHECK(def.effective_min_rowspan() <= d.rows);
    }
}

TEST_CASE("registry spans: a whole-cell widget spans a whole number of cells",
          "[widget_def][span_bands]") {
    // Edit mode snaps a widget with no half-cell flag to a TRACKS_PER_CELL step
    // on that axis (GridEditMode::snap_step_for), and the lattice only draws a
    // drop target on those boundaries. An authored odd span would therefore be a
    // size the user can never restore after one drag.
    constexpr int cell = GridLayout::TRACKS_PER_CELL;
    for (const auto& def : get_all_widget_defs()) {
        INFO("widget " << def.id);
        if (!def.supports_half_col) {
            CHECK(def.colspan % cell == 0);
            CHECK(def.effective_min_colspan() % cell == 0);
            CHECK(def.effective_max_colspan() % cell == 0);
        }
        if (!def.supports_half_row) {
            CHECK(def.rowspan % cell == 0);
            CHECK(def.effective_min_rowspan() % cell == 0);
            CHECK(def.effective_max_rowspan() % cell == 0);
        }
    }
}

TEST_CASE("registry spans: authored spans land in the intended pixel band",
          "[widget_def][span_bands]") {
    // {width band, height band} per widget, one entry per kGeometries row in
    // order. Regenerated from the run, then pinned: an authored span that moves
    // a widget across a threshold on ANY panel has to be an explicit edit here.
    //
    // Keyed by registry id. Widgets compiled out on this build (camera, behind
    // HELIX_HAS_CAMERA) simply never get looked up.
    //
    // Every row is uniform across the eight geometries, and that is the point
    // of the mechanism rather than a coincidence: a track is half a cell,
    // GridLayout::GRID_CELL keeps the cell growing with the type ladder, and
    // the bands are scaled off that same ladder - so an authored span lands in
    // one band, on every panel, in every orientation. A row that stops being
    // uniform is a widget whose span reads as one layout on one panel and a
    // different one elsewhere, which is exactly the surprise this file exists
    // to catch.
    using Bands = std::vector<std::pair<int, int>>;
    const std::map<std::string, Bands> expected = {
        {"printer_image", {{1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}}},
        {"print_status", {{1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}}},
        {"shutdown", {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}},
        {"lock", {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}},
        {"power_device", {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}},
        {"network", {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}},
        {"firmware_restart", {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}},
        {"tool_switcher", {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}},
        {"led", {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}},
        {"led_controls", {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}},
        {"fan_stack", {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}},
        {"fan", {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}},
        {"bypass", {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}},
        {"temperature", {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}},
        {"nozzle_temps", {{0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 1}}},
        {"bed_temperature", {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}},
        {"chamber_temperature", {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}},
        {"temp_stack", {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}},
        {"thermistor", {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}},
        {"temp_graph", {{1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}}},
        {"preheat", {{2, 0}, {2, 0}, {2, 0}, {2, 0}, {2, 0}, {2, 0}, {2, 0}, {2, 0}}},
        {"ams", {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}},
        {"active_spool", {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}},
        {"filament", {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}},
        {"humidity", {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}},
        {"width_sensor", {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}},
        {"favorite_macro", {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}},
        {"macros", {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}},
        {"motion", {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}},
        {"clock", {{1, 0}, {1, 0}, {1, 0}, {1, 0}, {1, 0}, {1, 0}, {1, 0}, {1, 0}}},
        {"control_buttons", {{1, 0}, {1, 0}, {1, 0}, {1, 0}, {1, 0}, {1, 0}, {1, 0}, {1, 0}}},
        {"job_queue", {{1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}}},
        {"tips", {{2, 1}, {2, 1}, {2, 1}, {2, 1}, {2, 1}, {2, 1}, {2, 1}, {2, 1}}},
        // 2x1 since #1017. Width band 1 (>= w_normal) on every geometry is the
        // point of that change: the FlowGuard bar puts an end label either side
        // of the scale, which is the same "room for two columns" threshold
        // control_buttons sits at — and is what the one-cell arc never had.
        {"clog_detection", {{1, 0}, {1, 0}, {1, 0}, {1, 0}, {1, 0}, {1, 0}, {1, 0}, {1, 0}}},
        {"filament_buffer", {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}},
        {"print_stats", {{1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}}},
        {"gcode_console", {{1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}}},
        {"camera", {{1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}}},
        {"notifications", {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}},
    };

    for (size_t gi = 0; gi < kGeometries.size(); ++gi) {
        const auto& g = kGeometries[gi];
        const CellMetrics m = metrics_for(g);

        for (const auto& def : get_all_widget_defs()) {
            const int w = extent_px(m.cell_w, m.gutter, def.colspan);
            const int h = extent_px(m.cell_h, m.gutter, def.rowspan);

            // Every registered widget must be pinned. A new one arrives with no
            // row here and fails, which is the prompt to decide what band its
            // span should land in rather than discovering it on a device.
            auto it = expected.find(def.id);
            INFO("widget " << def.id << " has no pinned bands");
            REQUIRE(it != expected.end());
            REQUIRE(it->second.size() == kGeometries.size());
            INFO(g.name << " " << def.id << " span " << def.colspan << "x" << def.rowspan << " -> "
                        << w << "x" << h << "px");
            CHECK(width_band(w, tier_of(g)) == it->second[gi].first);
            CHECK(height_band(h, tier_of(g)) == it->second[gi].second);
        }
    }
}
