// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_panel_buffer_box.cpp
 * @brief The AMS panel's path canvas buffer box follows the buffer reading
 *        while the panel is open, and a pressure with no set point is untinted.
 */

#include "ui_ams_sidebar.h"
#include "ui_ams_slot.h"
#include "ui_endless_spool_arrows.h"
#include "ui_filament_path_canvas.h"
#include "ui_panel_ams.h"
#include "ui_spool_canvas.h"

#include "../test_fixtures.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {
void set_pressure(AmsBackendMock& mock, float pressure, float set_point = 0.5f) {
    BufferHealth h;
    h.fps_value = h.smoothed_fps = pressure;
    h.fps_set_point = set_point;
    h.fps_reported = true;
    mock.set_unit_buffer_health(0, h);
    AmsState::instance().sync_from_backend();
}
} // namespace

TEST_CASE_METHOD(XMLTestFixture, "AmsPanel buffer box follows the reading while open",
                 "[ui_integration][ams][buffer][path]") {
    auto owned = std::make_unique<AmsBackendMock>(4);
    owned->set_afc_mode(true);
    REQUIRE(owned->start().success());
    auto* mock = owned.get();
    AmsState::instance().set_backend(std::move(owned));
    AmsState::instance().init_subjects(true);
    set_pressure(*mock, 0.52f);

    ensure_ams_widgets_registered();
    AmsPanel panel(state(), &api());
    panel.init_subjects();
    lv_obj_t* panel_obj =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "ams_panel", nullptr));
    REQUIRE(panel_obj != nullptr);
    panel.setup(panel_obj, test_screen());
    lv_obj_update_layout(test_screen());
    process_lvgl(50);

    lv_obj_t* canvas = lv_obj_find_by_name(panel.get_panel(), "path_canvas");
    REQUIRE(canvas != nullptr);

    SECTION("a reading that drifts tight turns the box to a fault") {
        CHECK(ui::filament_path_canvas_buffer_fault_state(canvas) == 0);
        set_pressure(*mock, 0.08f);
        process_lvgl(50);
        CHECK(ui::filament_path_canvas_buffer_fault_state(canvas) == 2);
    }

    SECTION("a pressure with no set point draws untinted") {
        set_pressure(*mock, 0.32f, -1.0f);
        process_lvgl(50);
        CHECK(ui::filament_path_canvas_buffer_fault_state(canvas) == -1);
    }

    panel.clear_panel_reference();
    lv_obj_delete(panel_obj);
    process_lvgl(10);
    AmsState::instance().set_backend(nullptr);
}
