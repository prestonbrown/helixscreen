// Copyright (C) 2025-2026 356C LLC
// TEST_MIRROR_OK: exercises patches/lvgl_grid_update_guard.patch — shipped LVGL code with no
// HelixScreen header SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_grid_cell_out_of_range.cpp
 * @brief Regression test for the out-of-range grid cell heap walk-off crash.
 *
 * item_repos() reads c->x[col_pos + col_span - 1] / c->y[row_pos + row_span - 1]
 * to position each grid child. calc() sizes those track arrays to col_num/row_num
 * only — it never validates that an item's cell actually fits the grid. A child
 * carrying a cell position/span beyond the track count (a still-valid object
 * whose grid-cell style was never set and reads back as a huge default, or a
 * position approved against a larger logical grid than the live descriptor) makes
 * item_repos() index past the array and walk off the heap end -> SIGSEGV.
 * Observed in production on an AD5X (MIPS32, v0.99.75, debug bundle P234RYCL):
 * crash inside grid_update -> item_repos with reg_v1=0x1fffffff (a garbage
 * ~half-billion cell index) and fault_addr exactly on the heap-end page boundary,
 * triggered while drag-editing dashboard widgets.
 *
 * The fix (patches/lvgl_grid_update_guard.patch) makes item_repos() skip — and
 * report via helix_lvgl_anomaly() — any item whose cell exceeds the calculated
 * track count. These tests pass simply by surviving the forced layout; a
 * regression would SIGSEGV the test process.
 *
 * @see lib/lvgl/src/layouts/grid/lv_grid.c item_repos()
 * @see test_grid_zero_track.cpp (sibling guard for the empty-template case)
 */

#include "../ui_test_utils.h"
#include "lvgl/lvgl.h"

#include <spdlog/spdlog.h>

#include "../catch_amalgamated.hpp"

// Grid templates are stored by pointer (not copied) by LVGL, so they must
// outlive the container — keep them at static storage duration.
static const int32_t TWO_FR_DSC[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
static const int32_t ONE_FR_DSC[] = {LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};

namespace {

bool g_lvgl_initialized = false;
lv_display_t* g_display = nullptr;
alignas(64) lv_color_t g_display_buf[800 * 10];

void ensure_lvgl_init() {
    if (!g_lvgl_initialized) {
        lv_init_safe();
        g_display = lv_display_create(800, 480);
        lv_display_set_buffers(g_display, g_display_buf, nullptr, sizeof(g_display_buf),
                               LV_DISPLAY_RENDER_MODE_PARTIAL);
        g_lvgl_initialized = true;
    }
}

} // namespace

class GridCellOutOfRangeFixture {
  public:
    lv_obj_t* screen = nullptr;

    GridCellOutOfRangeFixture() {
        ensure_lvgl_init();
        screen = lv_screen_active();
        lv_obj_clean(screen);
    }

    ~GridCellOutOfRangeFixture() {
        if (screen) {
            lv_obj_clean(screen);
        }
    }

    // Build a 2x1 grid and place one child at the given cell, then force the
    // layout pass that exercises item_repos().
    lv_obj_t* make_grid_with_cell(int32_t col, int32_t col_span, int32_t row, int32_t row_span) {
        lv_obj_t* grid = lv_obj_create(screen);
        lv_obj_set_size(grid, 400, 300);
        lv_obj_set_grid_dsc_array(grid, TWO_FR_DSC, ONE_FR_DSC); // 2 columns, 1 row
        lv_obj_set_layout(grid, LV_LAYOUT_GRID);

        lv_obj_t* child = lv_obj_create(grid);
        lv_obj_set_grid_cell(child, LV_GRID_ALIGN_STRETCH, col, col_span, LV_GRID_ALIGN_STRETCH,
                             row, row_span);
        return grid;
    }
};

TEST_CASE_METHOD(GridCellOutOfRangeFixture, "Grid child at out-of-range column does not crash",
                 "[grid][regression][cell_oob]") {
    // 2-column grid, child placed at column 5 -> c->x[5] is off the 2-element
    // array. Would SIGSEGV in item_repos before the fix.
    make_grid_with_cell(5, 1, 0, 1);
    lv_obj_update_layout(screen);
    SUCCEED("Survived layout with an out-of-range column position");
}

TEST_CASE_METHOD(GridCellOutOfRangeFixture, "Grid child with overflowing colspan does not crash",
                 "[grid][regression][cell_oob]") {
    // Valid start column but a span that runs off the end: col 1 + span 4 = 5 > 2.
    make_grid_with_cell(1, 4, 0, 1);
    lv_obj_update_layout(screen);
    SUCCEED("Survived layout with an overflowing column span");
}

TEST_CASE_METHOD(GridCellOutOfRangeFixture, "Grid child at out-of-range row does not crash",
                 "[grid][regression][cell_oob]") {
    // 1-row grid, child placed at row 9 -> c->y[9] is off the 1-element array.
    make_grid_with_cell(0, 1, 9, 1);
    lv_obj_update_layout(screen);
    SUCCEED("Survived layout with an out-of-range row position");
}

TEST_CASE_METHOD(GridCellOutOfRangeFixture, "In-range grid child still lays out",
                 "[grid][regression][cell_oob]") {
    // Guard against the fix over-rejecting: a child whose cell fits the tracks
    // must still be placed with a non-zero size.
    lv_obj_t* grid = lv_obj_create(screen);
    lv_obj_set_size(grid, 400, 300);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_grid_dsc_array(grid, TWO_FR_DSC, ONE_FR_DSC);
    lv_obj_set_layout(grid, LV_LAYOUT_GRID);

    lv_obj_t* child = lv_obj_create(grid);
    lv_obj_set_grid_cell(child, LV_GRID_ALIGN_STRETCH, 0, 2, LV_GRID_ALIGN_STRETCH, 0, 1);

    lv_obj_update_layout(screen);

    CHECK(lv_obj_get_width(child) > 0);
    CHECK(lv_obj_get_height(child) > 0);
}
