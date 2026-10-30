// Copyright (C) 2025-2026 356C LLC
// TEST_MIRROR_OK: exercises patches/lvgl_grid_update_guard.patch — shipped LVGL code with no
// HelixScreen header SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_grid_cell_out_of_range.cpp
 * @brief LVGL grid guards: out-of-range cells are skipped, zero-track templates do not crash.
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
 * track count. The tests give the child a size no track would stretch it to
 * and assert it keeps that size, so a missing guard fails the assertion even
 * where the over-read itself does not crash.
 *
 * The same patch makes calc_cols()/calc_rows() return LV_RESULT_INVALID on a
 * zero-track template ({LV_GRID_TEMPLATE_LAST} alone). Without it count_tracks()
 * returns 0, grid_align() evaluates `track_num - 1` as a uint32_t, and its
 * position loop walks ~4 billion entries off the heap (seen on a BIQU CB1 /
 * Voron 0.2 after a deferred thumbnail swap relaid a degenerate grid). Those
 * cases pass by surviving the forced layout: a regression SIGSEGVs the process.
 *
 * @see lib/lvgl/src/layouts/grid/lv_grid.c item_repos(), grid_align(), calc_cols()
 */

#include "../lvgl_test_fixture.h"
#include "../ui_test_utils.h"
#include "lvgl/lvgl.h"

#include <spdlog/spdlog.h>

#include "../catch_amalgamated.hpp"

// Grid templates are stored by pointer (not copied) by LVGL, so they must
// outlive the container — keep them at static storage duration.
static const int32_t kEmptyDsc[] = {LV_GRID_TEMPLATE_LAST};
static const int32_t kTwoFrDsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
static const int32_t kOneFrDsc[] = {LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};

class GridCellOutOfRangeFixture : public LVGLTestFixture {
  public:
    // An odd size no track of the 400x300 grid stretches a child to.
    static constexpr int32_t kChildW = 37;
    static constexpr int32_t kChildH = 23;

    lv_obj_t* screen = nullptr;

    GridCellOutOfRangeFixture() {
        screen = lv_screen_active();
        lv_obj_clean(screen);
    }

    ~GridCellOutOfRangeFixture() {
        if (screen) {
            lv_obj_clean(screen);
        }
    }

    // Build a 2x1 grid holding one kChildW x kChildH child at the given cell;
    // lv_obj_update_layout() then runs item_repos() over it.
    lv_obj_t* make_grid_with_cell(int32_t col, int32_t col_span, int32_t row, int32_t row_span) {
        lv_obj_t* grid = lv_obj_create(screen);
        lv_obj_set_size(grid, 400, 300);
        lv_obj_set_grid_dsc_array(grid, kTwoFrDsc, kOneFrDsc); // 2 columns, 1 row
        lv_obj_set_layout(grid, LV_LAYOUT_GRID);

        lv_obj_t* child = lv_obj_create(grid);
        lv_obj_set_size(child, kChildW, kChildH);
        lv_obj_set_grid_cell(child, LV_GRID_ALIGN_STRETCH, col, col_span, LV_GRID_ALIGN_STRETCH,
                             row, row_span);
        return grid;
    }

    // Build a grid with the given templates and one child cell. calc() bails
    // before count_tracks() unless the grid has a child, so the child is what
    // reaches the zero-track path.
    lv_obj_t* make_grid_with_templates(const int32_t* col_dsc, const int32_t* row_dsc) {
        lv_obj_t* grid = lv_obj_create(screen);
        lv_obj_set_size(grid, 400, 300);
        lv_obj_set_grid_dsc_array(grid, col_dsc, row_dsc);
        lv_obj_set_layout(grid, LV_LAYOUT_GRID);
        lv_obj_t* child = lv_obj_create(grid);
        lv_obj_set_grid_cell(child, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 0, 1);
        return grid;
    }

    // item_repos() skipped the child: no track stretched or moved it.
    static void expect_skipped(lv_obj_t* child) {
        REQUIRE(lv_obj_get_width(child) == kChildW);
        REQUIRE(lv_obj_get_height(child) == kChildH);
    }
};

TEST_CASE_METHOD(GridCellOutOfRangeFixture, "Grid child at out-of-range column does not crash",
                 "[grid][regression][cell_oob]") {
    // 2-column grid, child placed at column 5 -> c->x[5] is off the 2-element
    // array; item_repos() must skip the child rather than read it.
    lv_obj_t* grid = make_grid_with_cell(5, 1, 0, 1);
    lv_obj_update_layout(screen);
    expect_skipped(lv_obj_get_child(grid, 0));
}

TEST_CASE_METHOD(GridCellOutOfRangeFixture, "Grid child with overflowing colspan does not crash",
                 "[grid][regression][cell_oob]") {
    // Valid start column but a span that runs off the end: col 1 + span 4 = 5 > 2.
    lv_obj_t* grid = make_grid_with_cell(1, 4, 0, 1);
    lv_obj_update_layout(screen);
    expect_skipped(lv_obj_get_child(grid, 0));
}

TEST_CASE_METHOD(GridCellOutOfRangeFixture, "Grid child at out-of-range row does not crash",
                 "[grid][regression][cell_oob]") {
    // 1-row grid, child placed at row 9 -> c->y[9] is off the 1-element array.
    lv_obj_t* grid = make_grid_with_cell(0, 1, 9, 1);
    lv_obj_update_layout(screen);
    expect_skipped(lv_obj_get_child(grid, 0));
}

TEST_CASE_METHOD(GridCellOutOfRangeFixture, "In-range grid child still lays out",
                 "[grid][regression][cell_oob]") {
    // Guard against the fix over-rejecting: a child whose cell fits the tracks
    // must still be placed with a non-zero size.
    lv_obj_t* grid = lv_obj_create(screen);
    lv_obj_set_size(grid, 400, 300);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_grid_dsc_array(grid, kTwoFrDsc, kOneFrDsc);
    lv_obj_set_layout(grid, LV_LAYOUT_GRID);

    lv_obj_t* child = lv_obj_create(grid);
    lv_obj_set_grid_cell(child, LV_GRID_ALIGN_STRETCH, 0, 2, LV_GRID_ALIGN_STRETCH, 0, 1);

    lv_obj_update_layout(screen);

    CHECK(lv_obj_get_width(child) > 0);
    CHECK(lv_obj_get_height(child) > 0);
}

TEST_CASE_METHOD(GridCellOutOfRangeFixture, "Grid with empty column template does not crash",
                 "[grid][regression][zero_track]") {
    make_grid_with_templates(kEmptyDsc, kOneFrDsc);
    lv_obj_update_layout(screen);
    SUCCEED("Survived layout of a zero-column grid");
}

TEST_CASE_METHOD(GridCellOutOfRangeFixture, "Grid with empty row template does not crash",
                 "[grid][regression][zero_track]") {
    make_grid_with_templates(kOneFrDsc, kEmptyDsc);
    lv_obj_update_layout(screen);
    SUCCEED("Survived layout of a zero-row grid");
}

TEST_CASE_METHOD(GridCellOutOfRangeFixture, "Grid with both templates empty does not crash",
                 "[grid][regression][zero_track]") {
    make_grid_with_templates(kEmptyDsc, kEmptyDsc);
    lv_obj_update_layout(screen);
    SUCCEED("Survived layout of a zero-track grid");
}
