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

/// The global MotionPanel shown over a full panel set, as the controls screen
/// opens it; returns its root.
lv_obj_t* show_motion_panel() {
    std::array<lv_obj_t*, kUiPanelCount> panels{};
    for (auto& p : panels)
        p = lv_obj_create(lv_screen_active());
    NavigationManager::instance().set_panels(panels.data());
    REQUIRE(get_global_motion_panel().show(lv_screen_active()));
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_t* root = get_global_motion_panel().get_root();
    REQUIRE(root != nullptr);
    return root;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "unhomed axes mute their coordinate readouts",
                 "[motion][homed][xml]") {
    lv_obj_t* root = show_motion_panel();

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
    lv_subject_set_string(get_printer_state().motion_state().get_homed_axes_subject(), "xy");
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

    lv_obj_t* root = show_motion_panel();
    lv_subject_t* individual = lv_xml_get_subject(nullptr, "printer_has_individual_xyz_homing");
    REQUIRE(individual != nullptr);
    lv_subject_set_int(individual, 1);

    for (const char* name : {"btn_home_x", "btn_home_y", "btn_home_z"}) {
        CAPTURE(name);
        lv_obj_t* btn = lv_obj_find_by_name(root, name);
        REQUIRE(btn != nullptr);
        CHECK_FALSE(lv_obj_is_hidden(btn));
    }

    client.clear_gcode_script_history();
    lv_obj_send_event(lv_obj_find_by_name(root, "btn_home_x"), LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain();
    const auto& h = client.gcode_script_history();
    CHECK(std::find(h.begin(), h.end(), "G28 X") != h.end());

    lv_subject_set_int(individual, 0);
    CHECK(lv_obj_is_hidden(lv_obj_find_by_name(root, "btn_home_x")));

    StaticPanelRegistry::instance().destroy_all();
    helix::ui::UpdateQueue::instance().drain();
    set_moonraker_api(previous);
}

TEST_CASE_METHOD(LVGLUITestFixture, "the Z column draws what the Z keys move", "[motion][xml]") {
    lv_obj_t* root = show_motion_panel();
    lv_obj_t* head = lv_obj_find_by_name(root, "z_axis_icon_head");
    lv_obj_t* bed = lv_obj_find_by_name(root, "z_axis_icon_bed");
    REQUIRE(head != nullptr);
    REQUIRE(bed != nullptr);
    lv_subject_t* bed_moves = lv_xml_get_subject(nullptr, "printer_bed_moves");
    REQUIRE(bed_moves != nullptr);

    lv_subject_set_int(bed_moves, 0);
    CHECK_FALSE(lv_obj_is_hidden(head));
    CHECK(lv_obj_is_hidden(bed));
    lv_subject_set_int(bed_moves, 1);
    CHECK(lv_obj_is_hidden(head));
    CHECK_FALSE(lv_obj_is_hidden(bed));

    StaticPanelRegistry::instance().destroy_all();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE("the motion home bar trades All for XY when five segments do not fit",
          "[motion][homed]") {
    using helix::kHomeBarSegmentsWithXy;
    using helix::motion_home_bar_segments;
    constexpr int32_t kMin = 48;
    auto shows = [](helix::HomeBarSegments s, bool all, bool xy) {
        return s.all == all && s.xy == xy;
    };
    // Room for five touch targets: All, X, Y, XY, Z.
    CHECK(shows(motion_home_bar_segments(true, kHomeBarSegmentsWithXy * kMin, kMin), true, true));
    // One pixel short: the jog pad's center button homes all, so All goes.
    CHECK(shows(motion_home_bar_segments(true, kHomeBarSegmentsWithXy * kMin - 1, kMin), false,
                true));
    CHECK(shows(motion_home_bar_segments(true, 196, kMin), false, true));
    // A printer that homes every axis together keeps All alone, at any width.
    CHECK(shows(motion_home_bar_segments(false, 196, kMin), true, false));
    CHECK(shows(motion_home_bar_segments(false, 1000, kMin), true, false));
    // An unknown minimum is never room for five.
    CHECK(shows(motion_home_bar_segments(true, 1000, 0), false, true));
}

TEST_CASE_METHOD(LVGLUITestFixture, "the motion home bar fits its segments to its width",
                 "[motion][homed][xml]") {
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

    lv_obj_t* bar = lv_obj_find_by_name(root, "motion_home_bar");
    lv_obj_t* all = lv_obj_find_by_name(root, "btn_home_all");
    lv_obj_t* xy = lv_obj_find_by_name(root, "btn_home_xy");
    REQUIRE(bar != nullptr);
    REQUIRE(all != nullptr);
    REQUIRE(xy != nullptr);
    const int32_t min_segment = theme_manager_get_spacing("touch_target_min");
    REQUIRE(min_segment > 0);

    // The bar's own SIZE_CHANGED event decides; nothing else is called.
    auto resize = [&](int32_t content_w) {
        lv_obj_set_flex_grow(bar, 0);
        lv_obj_set_width(bar, content_w + lv_obj_get_style_pad_left(bar, LV_PART_MAIN) +
                                  lv_obj_get_style_pad_right(bar, LV_PART_MAIN) +
                                  2 * lv_obj_get_style_border_width(bar, LV_PART_MAIN));
        lv_obj_update_layout(bar);
        helix::ui::UpdateQueue::instance().drain();
    };

    resize(helix::kHomeBarSegmentsWithXy * min_segment + 40);
    CHECK_FALSE(lv_obj_is_hidden(all));
    CHECK_FALSE(lv_obj_is_hidden(xy));

    client.clear_gcode_script_history();
    lv_obj_send_event(xy, LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain();
    const auto& h = client.gcode_script_history();
    CHECK(std::find(h.begin(), h.end(), "G28 X Y") != h.end());

    // Too narrow for five: XY, X, Y, Z; the jog pad's center button homes all.
    resize(helix::kHomeBarSegmentsWithXy * min_segment - 10);
    CHECK(lv_obj_is_hidden(all));
    CHECK_FALSE(lv_obj_is_hidden(xy));
    CHECK_FALSE(lv_obj_is_hidden(lv_obj_find_by_name(root, "btn_home_z")));

    // A printer that cannot home axes separately shows All alone, however narrow.
    lv_subject_set_int(individual, 0);
    CHECK_FALSE(lv_obj_is_hidden(all));
    CHECK(lv_obj_is_hidden(xy));

    StaticPanelRegistry::instance().destroy_all();
    helix::ui::UpdateQueue::instance().drain();
    set_moonraker_api(previous);
}
