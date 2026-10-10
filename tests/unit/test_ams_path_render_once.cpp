// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_path_render_once.cpp
 * @brief Opening the AMS panel paints the filament path once, at its final size.
 *
 * A full topology repaint is the costliest draw on the panel, and an open drives
 * the canvas through a hidden build, an activation that resizes it, and several
 * state pushes. Only the state the user can see needs painting.
 */

#include "ui_ams_detail.h"
#include "ui_filament_path_canvas.h"
#include "ui_panel_ams.h"

#include "../lvgl_test_fixture.h"
#include "../test_fixtures.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "src/ui/ui_filament_path_internal.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE_METHOD(XMLTestFixture, "AmsPanel open paints the filament path once",
                 "[ui_integration][ams][filament_path][render_once]") {
    auto owned = std::make_unique<AmsBackendMock>(4);
    owned->set_afc_mode(true);
    REQUIRE(owned->start().success());
    AmsState::instance().set_backend(std::move(owned));
    AmsState::instance().init_subjects(true);
    AmsState::instance().sync_from_backend();

    // Built hidden, the way get_global_ams_panel() builds it before the push.
    ensure_ams_widgets_registered();
    AmsPanel panel(state(), &api());
    panel.init_subjects();
    lv_obj_t* panel_obj =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "ams_panel", nullptr));
    REQUIRE(panel_obj != nullptr);
    panel.setup(panel_obj, test_screen());
    lv_obj_add_flag(panel_obj, LV_OBJ_FLAG_HIDDEN);
    process_lvgl(100);

    lv_obj_t* canvas = lv_obj_find_by_name(panel.get_panel(), "path_canvas");
    REQUIRE(canvas != nullptr);
    auto* data = ui::fpath::get_data(canvas);
    REQUIRE(data != nullptr);
    CHECK(data->layers.render_count == 0);

    // The push shows it and activates it.
    lv_obj_remove_flag(panel_obj, LV_OBJ_FLAG_HIDDEN);
    panel.on_activate();
    process_lvgl(200);

    CHECK(data->layers.render_count == 1);
    REQUIRE(data->layers.overlay_buf != nullptr);
    CHECK(static_cast<int32_t>(data->layers.overlay_buf->header.w) == lv_obj_get_width(canvas));

    SECTION("a backend push that changes nothing paints nothing") {
        // What every path observer the panel holds does on each notification.
        ams_detail_setup_path_canvas(canvas, lv_obj_find_by_name(panel.get_panel(), "slot_grid"),
                                     -1);
        process_lvgl(100);
        CHECK(data->layers.render_count == 1);
    }

    SECTION("a state change after the open still paints") {
        ui_filament_path_canvas_set_filament_color(canvas, 0x12AB34);
        process_lvgl(100);
        CHECK(data->layers.render_count == 2);
    }

    SECTION("a later resize paints at the new size") {
        lv_obj_set_width(canvas, lv_obj_get_width(canvas) - 40);
        lv_obj_update_layout(canvas);
        process_lvgl(100);
        CHECK(data->layers.render_count == 2);
        REQUIRE(data->layers.overlay_buf != nullptr);
        CHECK(static_cast<int32_t>(data->layers.overlay_buf->header.w) == lv_obj_get_width(canvas));
    }

    panel.clear_panel_reference();
    lv_obj_delete(panel_obj);
    process_lvgl(10);
    AmsState::instance().set_backend(nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "FilamentPath: a canvas shown with no state change still paints",
                 "[filament_path][render_once]") {
    lv_obj_t* box = lv_obj_create(test_screen());
    lv_obj_set_size(box, 600, 400);
    lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t* path = ui_filament_path_canvas_create(box);
    REQUIRE(path != nullptr);
    lv_obj_set_size(path, 470, 294);
    lv_obj_update_layout(box);
    process_lvgl(100);

    auto* data = ui::fpath::get_data(path);
    REQUIRE(data != nullptr);
    CHECK(data->layers.render_count == 0);

    // Shown by its parent; nothing about the path itself changes. A drawn
    // frame is what notices.
    lv_obj_remove_flag(box, LV_OBJ_FLAG_HIDDEN);
    lv_refr_now(nullptr);
    process_lvgl(100);
    CHECK(data->layers.render_count == 1);

    lv_obj_delete(box);
    process_lvgl(30);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "FilamentPath: a state change while hidden paints once when shown",
                 "[filament_path][render_once]") {
    lv_obj_t* box = lv_obj_create(test_screen());
    lv_obj_set_size(box, 600, 400);
    lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t* path = ui_filament_path_canvas_create(box);
    REQUIRE(path != nullptr);
    lv_obj_set_size(path, 470, 294);
    lv_obj_update_layout(box);
    process_lvgl(100);

    auto* data = ui::fpath::get_data(path);
    REQUIRE(data != nullptr);
    ui_filament_path_canvas_set_filament_color(path, 0x12AB34);
    ui_filament_path_canvas_set_slot_count(path, 6);
    process_lvgl(100);
    CHECK(data->layers.render_count == 0);

    lv_obj_remove_flag(box, LV_OBJ_FLAG_HIDDEN);
    lv_refr_now(nullptr);
    process_lvgl(100);
    CHECK(data->layers.render_count == 1);
    CHECK(data->filament_color == 0x12AB34u);
    CHECK(data->slot_count == 6);
    CHECK_FALSE(data->layers.overlay_dirty);

    lv_obj_delete(box);
    process_lvgl(30);
}

TEST_CASE_METHOD(LVGLTestFixture, "FilamentPath: a different slot grid repaints the lanes",
                 "[filament_path][render_once]") {
    lv_obj_t* path = ui_filament_path_canvas_create(test_screen());
    REQUIRE(path != nullptr);
    lv_obj_set_size(path, 470, 294);
    lv_obj_update_layout(path);
    process_lvgl(100);
    auto* data = ui::fpath::get_data(path);
    REQUIRE(data != nullptr);

    auto make_grid = [&](int slots) {
        lv_obj_t* grid = lv_obj_create(test_screen());
        for (int i = 0; i < slots; i++) {
            lv_obj_t* slot = lv_obj_create(grid);
            lv_obj_set_name(lv_obj_create(slot), "spool_container");
        }
        return grid;
    };
    lv_obj_t* grid_a = make_grid(4);
    lv_obj_t* grid_b = make_grid(4);

    ui_filament_path_canvas_set_slot_grid(path, grid_a);
    process_lvgl(100);
    const int after_a = data->layers.render_count;
    REQUIRE(after_a >= 1);

    // The same grid again is no change.
    ui_filament_path_canvas_set_slot_grid(path, grid_a);
    process_lvgl(100);
    CHECK(data->layers.render_count == after_a);

    // Another unit's grid, same slot count: the lanes start somewhere else.
    ui_filament_path_canvas_set_slot_grid(path, grid_b);
    process_lvgl(100);
    CHECK(data->layers.render_count == after_a + 1);

    lv_obj_delete(path);
    lv_obj_delete(grid_a);
    lv_obj_delete(grid_b);
    process_lvgl(30);
}
