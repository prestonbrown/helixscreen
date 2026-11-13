// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_breakpoint.h"

#include "config.h"
#include "grid_layout.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"
#include "theme_manager.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace helix {

/// Container + one named child widget + a matching config page, sized so
/// current_metrics() reports real cell geometry. Mirrors the setup in
/// test_grid_edit_drag_path.cpp, minus the synthetic indev, for tests that drive
/// GridEditMode's private steps directly through GridEditModeTestAccess.
struct GridEditScene {
    /// The widget's span: one authored cell on each axis, which is
    /// TRACKS_PER_CELL tracks. Spans are in tracks everywhere.
    static constexpr int kColspan = GridLayout::TRACKS_PER_CELL;
    static constexpr int kRowspan = GridLayout::TRACKS_PER_CELL;

    lv_obj_t* container = nullptr;
    lv_obj_t* widget = nullptr;
    PanelWidgetConfig* config = nullptr;
    std::string panel_id;

    // lv_obj_set_grid_dsc_array() stores the POINTER, so these must outlive
    // the container. As constructor locals they dangled, and current_metrics()
    // read off the end of them on every enter() (caught by valgrind).
    std::vector<int32_t> col_dsc;
    std::vector<int32_t> row_dsc;

    GridEditScene(lv_obj_t* parent, std::string id) : panel_id(std::move(id)) {
        const int gutter = theme_manager_get_spacing("space_xs");
        REQUIRE(gutter > 0);

        // The content box decides the track count, so it is fixed first and the
        // grid derived from it — same geometry test_grid_edit_drag_path.cpp
        // uses. 715x475 is the one size near this fixture's display that gives
        // Medium a 12x8 grid AND divides into exact 55px tracks, so there is no
        // LVGL remainder distribution to muddy the pixel arithmetic.
        constexpr int kCellPx = 55;
        constexpr int content_w = 715;
        constexpr int content_h = 475;
        const auto dims = GridLayout::get_dimensions(UiBreakpoint::Medium, content_w, content_h);
        const int ncols = dims.cols;
        const int nrows = dims.rows;
        REQUIRE(ncols == 12);
        REQUIRE(nrows == 8);
        REQUIRE(content_w == ncols * kCellPx + (ncols - 1) * gutter);
        REQUIRE(content_h == nrows * kCellPx + (nrows - 1) * gutter);

        container = lv_obj_create(parent);
        lv_obj_set_scrollable(container, false);
        lv_obj_set_style_pad_all(container, 0, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_size(container, content_w, content_h);

        col_dsc = GridLayout::make_col_dsc(ncols);
        row_dsc = GridLayout::make_row_dsc(nrows);
        lv_obj_set_grid_dsc_array(container, col_dsc.data(), row_dsc.data());
        lv_obj_set_style_pad_column(container, gutter, 0);
        lv_obj_set_style_pad_row(container, gutter, 0);

        widget = lv_obj_create(container);
        lv_obj_set_name(widget, "temperature");
        lv_obj_set_scrollable(widget, false);
        lv_obj_set_grid_cell(widget, LV_GRID_ALIGN_STRETCH, 0, kColspan, LV_GRID_ALIGN_STRETCH, 0,
                             kRowspan);
        lv_obj_update_layout(container);

        // Widget lives on page 1: PanelWidgetConfig::load() appends registry
        // defaults onto page 0 only, so a second page keeps the entry list
        // exactly what this test wrote.
        auto* cfg = Config::get_instance();
        cfg->set<nlohmann::json>(
            cfg->df() + "panel_widgets/" + panel_id,
            nlohmann::json{{"main_page_index", 0},
                           {"next_page_id", 2},
                           {"pages",
                            {{{"id", "main"}, {"widgets", nlohmann::json::array()}},
                             {{"id", "spy"},
                              {"widgets",
                               {{{"id", "temperature"},
                                 {"enabled", true},
                                 {"col", 0},
                                 {"row", 0},
                                 {"colspan", kColspan},
                                 {"rowspan", kRowspan}}}}}}}});

        auto& mgr = PanelWidgetManager::instance();
        mgr.get_widget_config(panel_id).mark_dirty();
        mgr.clear_panel_config(panel_id);
        config = &mgr.get_widget_config(panel_id);

        // A widget ID parse_widget_array() doesn't recognise is dropped
        // silently, which would make commit_resize_with_snap() bail at its
        // find_config_index_for_widget() guard and never start an animation.
        REQUIRE(config->page_entries(kPageIndex).size() == 1);
    }

    ~GridEditScene() {
        PanelWidgetManager::instance().clear_panel_config(panel_id);
    }

    GridEditScene(const GridEditScene&) = delete;
    GridEditScene& operator=(const GridEditScene&) = delete;

    static constexpr size_t kPageIndex = 1;
};

} // namespace helix
