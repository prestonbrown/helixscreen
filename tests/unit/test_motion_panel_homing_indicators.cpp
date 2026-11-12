// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_motion_panel_homing_indicators.cpp
 * @brief An unhomed axis reads differently from a homed one at 0.00
 *
 * Run with: ./build/bin/helix-tests "[motion][homed]"
 *
 * The coordinate readouts mute their letter and value while an axis is
 * unhomed (motion_x/y/z_homed subjects, parsed by MotionPanel's homed_axes
 * observer): an unhomed axis reports 0.00, which must not read as a real
 * position. The muted look is a declarative bind_style pair on the
 * header_pos_* and row_pos_* labels, so the test drives a real MotionPanel
 * through the same OverlayBase::show path the controls screen
 * uses, flips homed_axes to "xy", and compares the label colors against the
 * theme tokens the styles name.
 */

#include "ui_nav_manager.h"
#include "ui_panel_motion.h"

#include "../lvgl_ui_test_fixture.h"
#include "app_globals.h"
#include "core/static_panel_registry.h"
#include "core/update_queue.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "theme_manager.h"

#include <algorithm>
#include <array>
#include <lvgl.h>

#include "../catch_amalgamated.hpp"

namespace {

bool same_color(const lv_color_t& a, const lv_color_t& b) {
    return a.red == b.red && a.green == b.green && a.blue == b.blue;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "unhomed axes mute their coordinate readouts",
                 "[motion][homed][xml]") {
    std::array<lv_obj_t*, kUiPanelCount> panels{};
    for (auto& p : panels)
        p = lv_obj_create(lv_screen_active());
    NavigationManager::instance().set_panels(panels.data());

    lv_obj_t* cached = nullptr;
    REQUIRE(get_global_motion_panel().show(lv_screen_active()));
    cached = get_global_motion_panel().get_root();
    helix::ui::UpdateQueue::instance().drain();

    lv_obj_t* root = get_global_motion_panel().get_root();
    REQUIRE(root != nullptr);

    // The landscape branch renders header_pos_* (the portrait strip's
    // row_pos_* labels carry the same binding pair).
    lv_obj_t* x_value = lv_obj_find_by_name(root, "header_pos_x");
    lv_obj_t* x_letter = lv_obj_find_by_name(root, "header_pos_x_letter");
    lv_obj_t* z_value = lv_obj_find_by_name(root, "header_pos_z");
    lv_obj_t* z_letter = lv_obj_find_by_name(root, "header_pos_z_letter");
    REQUIRE(x_value != nullptr);
    REQUIRE(x_letter != nullptr);
    REQUIRE(z_value != nullptr);
    REQUIRE(z_letter != nullptr);

    // Sanity: before any homing report every axis reads muted.
    const lv_color_t muted = theme_manager_get_color("text_muted");
    const lv_color_t value = theme_manager_get_color("text");
    const lv_color_t letter = theme_manager_get_color("primary");
    CHECK(same_color(lv_obj_get_style_text_color(x_value, LV_PART_MAIN), muted));

    // X and Y homed, Z left unhomed.
    lv_subject_copy_string(get_printer_state().motion_state().get_homed_axes_subject(), "xy");
    helix::ui::UpdateQueue::instance().drain();

    CHECK(same_color(lv_obj_get_style_text_color(x_value, LV_PART_MAIN), value));
    CHECK(same_color(lv_obj_get_style_text_color(x_letter, LV_PART_MAIN), letter));
    CHECK(same_color(lv_obj_get_style_text_color(z_value, LV_PART_MAIN), muted));
    CHECK(same_color(lv_obj_get_style_text_color(z_letter, LV_PART_MAIN), muted));

    StaticPanelRegistry::instance().destroy_all();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(LVGLUITestFixture, "the motion panel homes single axes",
                 "[motion][homed][xml][target_tool]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    state.set_klippy_state_sync(helix::KlippyState::READY);
    MoonrakerAPI api(client, state);
    IMoonrakerAPI* previous = get_moonraker_api();
    set_moonraker_api(&api);

    std::array<lv_obj_t*, kUiPanelCount> panels{};
    for (auto& p : panels)
        p = lv_obj_create(lv_screen_active());
    NavigationManager::instance().set_panels(panels.data());
    REQUIRE(get_global_motion_panel().show(lv_screen_active()));
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_t* root = get_global_motion_panel().get_root();
    REQUIRE(root != nullptr);
    lv_subject_t* individual = lv_xml_get_subject(nullptr, "printer_has_individual_xyz_homing");
    REQUIRE(individual != nullptr);
    lv_subject_set_int(individual, 1);

    for (const char* name : {"btn_home_all", "btn_home_x", "btn_home_y", "btn_home_z"}) {
        CAPTURE(name);
        lv_obj_t* btn = lv_obj_find_by_name(root, name);
        REQUIRE(btn != nullptr);
        CHECK_FALSE(lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN));
    }
    lv_obj_t* xy = lv_obj_find_by_name(root, "btn_home_xy");
    REQUIRE(xy != nullptr);
    CHECK(lv_obj_has_flag(xy, LV_OBJ_FLAG_HIDDEN));

    client.clear_gcode_script_history();
    lv_obj_send_event(lv_obj_find_by_name(root, "btn_home_x"), LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain();
    const auto& h = client.gcode_script_history();
    CHECK(std::find(h.begin(), h.end(), "G28 X") != h.end());

    lv_subject_set_int(individual, 0);
    CHECK(lv_obj_has_flag(lv_obj_find_by_name(root, "btn_home_x"), LV_OBJ_FLAG_HIDDEN));

    StaticPanelRegistry::instance().destroy_all();
    helix::ui::UpdateQueue::instance().drain();
    set_moonraker_api(previous);
}

TEST_CASE_METHOD(LVGLUITestFixture, "the Z column draws what the Z keys move", "[motion][xml]") {
    std::array<lv_obj_t*, kUiPanelCount> panels{};
    for (auto& p : panels)
        p = lv_obj_create(lv_screen_active());
    NavigationManager::instance().set_panels(panels.data());
    REQUIRE(get_global_motion_panel().show(lv_screen_active()));
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_t* root = get_global_motion_panel().get_root();
    REQUIRE(root != nullptr);
    lv_obj_t* head = lv_obj_find_by_name(root, "z_axis_icon_head");
    lv_obj_t* bed = lv_obj_find_by_name(root, "z_axis_icon_bed");
    REQUIRE(head != nullptr);
    REQUIRE(bed != nullptr);
    lv_subject_t* bed_moves = lv_xml_get_subject(nullptr, "printer_bed_moves");
    REQUIRE(bed_moves != nullptr);

    lv_subject_set_int(bed_moves, 0);
    CHECK_FALSE(lv_obj_has_flag(head, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(bed, LV_OBJ_FLAG_HIDDEN));
    lv_subject_set_int(bed_moves, 1);
    CHECK(lv_obj_has_flag(head, LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(bed, LV_OBJ_FLAG_HIDDEN));

    StaticPanelRegistry::instance().destroy_all();
    helix::ui::UpdateQueue::instance().drain();
}
