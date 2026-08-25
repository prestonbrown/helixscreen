// Copyright (C) 2025-2026 356C LLC
// TEST_MIRROR_OK: exercises patches/lvgl_grid_update_guard.patch — shipped LVGL code with no
// HelixScreen header SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_grid_zero_track.cpp
 * @brief Regression test for the zero-track grid_align() underflow crash.
 *
 * A grid container whose column (or row) template is empty —
 * {LV_GRID_TEMPLATE_LAST} with no tracks — makes count_tracks() return 0.
 * grid_align() then evaluates `track_num - 1` as a uint32_t, underflowing to
 * 0xFFFFFFFF, and its position loop writes ~4 billion entries past the track
 * arrays, walking off the heap end -> SIGSEGV. Observed in production on a
 * BIQU CB1 / Voron 0.2 (debug bundle FHWM8JVG, v0.99.74): a deferred
 * PrintStatusWidget thumbnail swap triggered a relayout of a degenerate grid.
 *
 * The fix (patches/lvgl_grid_update_guard.patch) makes calc_cols()/calc_rows()
 * return LV_RESULT_INVALID on a zero-track template, so grid_update() skips the
 * grid entirely. These tests pass simply by surviving the forced layout — a
 * regression would SIGSEGV the test process.
 *
 * @see lib/lvgl/src/layouts/grid/lv_grid.c grid_align(), calc_cols(), calc_rows()
 */

#include "../ui_test_utils.h"
#include "lvgl/lvgl.h"

#include <spdlog/spdlog.h>

#include "../catch_amalgamated.hpp"

// Grid templates are stored by pointer (not copied) by LVGL, so they must
// outlive the container — keep them at static storage duration.
static const int32_t EMPTY_DSC[] = {LV_GRID_TEMPLATE_LAST};
static const int32_t ONE_FR_DSC[] = {LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
static const int32_t TWO_FR_DSC[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};

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

class GridZeroTrackFixture {
  public:
    lv_obj_t* screen = nullptr;

    GridZeroTrackFixture() {
        ensure_lvgl_init();
        screen = lv_screen_active();
        lv_obj_clean(screen);
    }

    ~GridZeroTrackFixture() {
        if (screen) {
            lv_obj_clean(screen);
        }
    }

    // Build a grid container with the given templates and one child cell, then
    // force the layout pass that exercises grid_align().
    lv_obj_t* make_grid(const int32_t* col_dsc, const int32_t* row_dsc) {
        lv_obj_t* grid = lv_obj_create(screen);
        lv_obj_set_size(grid, 400, 300);
        lv_obj_set_grid_dsc_array(grid, col_dsc, row_dsc);
        lv_obj_set_layout(grid, LV_LAYOUT_GRID);

        // calc() bails before count_tracks() unless the grid has a child, so a
        // child is required to actually reach the underflow path.
        lv_obj_t* child = lv_obj_create(grid);
        lv_obj_set_grid_cell(child, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 0, 1);
        return grid;
    }
};

TEST_CASE_METHOD(GridZeroTrackFixture, "Grid with empty column template does not crash",
                 "[grid][regression][zero_track]") {
    make_grid(EMPTY_DSC, ONE_FR_DSC);
    lv_obj_update_layout(screen); // would SIGSEGV before the fix
    SUCCEED("Survived layout of a zero-column grid");
}

TEST_CASE_METHOD(GridZeroTrackFixture, "Grid with empty row template does not crash",
                 "[grid][regression][zero_track]") {
    make_grid(ONE_FR_DSC, EMPTY_DSC);
    lv_obj_update_layout(screen);
    SUCCEED("Survived layout of a zero-row grid");
}

TEST_CASE_METHOD(GridZeroTrackFixture, "Grid with both templates empty does not crash",
                 "[grid][regression][zero_track]") {
    make_grid(EMPTY_DSC, EMPTY_DSC);
    lv_obj_update_layout(screen);
    SUCCEED("Survived layout of a zero-track grid");
}

TEST_CASE_METHOD(GridZeroTrackFixture, "Valid grid still lays out children",
                 "[grid][regression][zero_track]") {
    // Guard against the fix over-rejecting: a well-formed grid must still place
    // its child with a non-zero size.
    lv_obj_t* grid = lv_obj_create(screen);
    lv_obj_set_size(grid, 400, 300);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_grid_dsc_array(grid, TWO_FR_DSC, ONE_FR_DSC);
    lv_obj_set_layout(grid, LV_LAYOUT_GRID);

    lv_obj_t* child = lv_obj_create(grid);
    lv_obj_set_grid_cell(child, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 0, 1);

    lv_obj_update_layout(screen);

    CHECK(lv_obj_get_width(child) > 0);
    CHECK(lv_obj_get_height(child) > 0);
}
