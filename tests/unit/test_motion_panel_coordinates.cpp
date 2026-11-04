// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_motion_panel_coordinates.cpp
 * @brief The coordinate readout renders commanded or actual (live) position
 *
 * Run with: ./build/bin/helix-tests "[motion][coords]"
 *
 * The header readouts (motion_pos_x/y/z subjects) show the commanded gcode
 * position by default and the live position while the persisted preference
 * says "actual". Flipping the SettingsManager subject re-renders through the
 * panel's coordinate-mode observer, and the swap glyph recolors with it.
 * keypad_params_for_axis() decides the keypad's seed and bounds from the axis
 * envelope: always the commanded seed, negative input only where the axis
 * minimum is below zero, nothing when the envelope is unknown.
 */

#include "ui_nav_manager.h"
#include "ui_panel_motion.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/log_capture.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "hold_repeat.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_motion_state.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "static_panel_registry.h"
#include "theme_manager.h"
#include "unit_conversions.h"

#include <array>
#include <cstring>
#include <lvgl.h>

#include "../catch_amalgamated.hpp"

namespace {

bool same_color(const lv_color_t& a, const lv_color_t& b) {
    return a.red == b.red && a.green == b.green && a.blue == b.blue;
}

void check_header_text(lv_obj_t* root, const char* name, const char* expected) {
    lv_obj_t* label = lv_obj_find_by_name(root, name);
    REQUIRE(label != nullptr);
    CHECK(std::strcmp(lv_label_get_text(label), expected) == 0);
}

} // namespace

TEST_CASE("keypad params seed the commanded value and bound by the envelope", "[motion][coords]") {
    helix::AxisBounds bounds;
    bounds.has_x = true;
    bounds.x_min = 0.0f;
    bounds.x_max = 350.0f;
    bounds.has_z = true;
    bounds.z_min = -5.0f;
    bounds.z_max = 200.0f;

    const auto x = helix::keypad_params_for_axis(bounds, helix::Axis::X, 123.4);
    REQUIRE(x.has_value());
    CHECK(x->seed == Catch::Approx(123.4f));
    CHECK(x->min_value == Catch::Approx(0.0f));
    CHECK(x->max_value == Catch::Approx(350.0f));
    CHECK_FALSE(x->allow_negative);

    // Z's minimum is below zero, so negative input is allowed there.
    const auto z = helix::keypad_params_for_axis(bounds, helix::Axis::Z, -1.25);
    REQUIRE(z.has_value());
    CHECK(z->seed == Catch::Approx(-1.25f));
    CHECK(z->min_value == Catch::Approx(-5.0f));
    CHECK(z->max_value == Catch::Approx(200.0f));
    CHECK(z->allow_negative);

    // Y has no known envelope: an absolute move would have nothing to clamp
    // against, so there is nothing to open a keypad with.
    CHECK_FALSE(helix::keypad_params_for_axis(bounds, helix::Axis::Y, 10.0).has_value());
}

TEST_CASE_METHOD(LVGLUITestFixture, "coordinate readouts follow the commanded/actual preference",
                 "[motion][coords][xml]") {
    std::array<lv_obj_t*, kUiPanelCount> panels{};
    for (auto& p : panels)
        p = lv_obj_create(lv_screen_active());
    NavigationManager::instance().set_panels(panels.data());

    auto& settings = helix::SettingsManager::instance();
    settings.init_subjects();
    settings.set_motion_show_actual_position(false);

    lv_obj_t* cached = nullptr;
    REQUIRE(get_global_motion_panel().show(lv_screen_active()));
    cached = get_global_motion_panel().get_root();
    helix::ui::UpdateQueue::instance().drain();

    lv_obj_t* root = get_global_motion_panel().get_root();
    REQUIRE(root != nullptr);

    auto& ps = get_printer_state();
    using helix::units::to_centimm;
    lv_subject_set_int(ps.motion_state().get_gcode_position_x_subject(), to_centimm(10.0));
    lv_subject_set_int(ps.motion_state().get_gcode_position_y_subject(), to_centimm(20.0));
    lv_subject_set_int(ps.motion_state().get_gcode_position_z_subject(), to_centimm(5.0));
    lv_subject_set_int(ps.motion_state().get_live_position_x_subject(), to_centimm(11.5));
    lv_subject_set_int(ps.motion_state().get_live_position_y_subject(), to_centimm(21.5));
    lv_subject_set_int(ps.motion_state().get_live_position_z_subject(), to_centimm(5.25));
    helix::ui::UpdateQueue::instance().drain();

    // Commanded mode (the default): gcode position, not live.
    check_header_text(root, "header_pos_x", "10.00");
    check_header_text(root, "header_pos_y", "20.00");
    check_header_text(root, "header_pos_z", "5.00");

    // The swap glyph reads muted while the preference is "commanded".
    lv_obj_t* swap = lv_obj_find_by_name(root, "header_pos_swap");
    REQUIRE(swap != nullptr);
    lv_obj_t* glyph = lv_obj_get_child(swap, 0);
    REQUIRE(glyph != nullptr);
    CHECK(same_color(lv_obj_get_style_text_color(glyph, LV_PART_MAIN),
                     theme_manager_get_color("text_muted")));

    settings.set_motion_show_actual_position(true);
    helix::ui::UpdateQueue::instance().drain();

    // Actual mode: live position, re-rendered by the flip.
    check_header_text(root, "header_pos_x", "11.50");
    check_header_text(root, "header_pos_y", "21.50");
    check_header_text(root, "header_pos_z", "5.25");
    CHECK(same_color(lv_obj_get_style_text_color(glyph, LV_PART_MAIN),
                     theme_manager_get_color("primary")));

    // Live updates keep flowing while the preference is "actual"...
    lv_subject_set_int(ps.motion_state().get_live_position_x_subject(), to_centimm(12.75));
    helix::ui::UpdateQueue::instance().drain();
    check_header_text(root, "header_pos_x", "12.75");

    // ...and commanded updates win again once it flips back.
    settings.set_motion_show_actual_position(false);
    lv_subject_set_int(ps.motion_state().get_gcode_position_x_subject(), to_centimm(13.0));
    helix::ui::UpdateQueue::instance().drain();
    check_header_text(root, "header_pos_x", "13.00");

    StaticPanelRegistry::instance().destroy_all();
    helix::ui::UpdateQueue::instance().drain();
}

// ============================================================================
// G-code space: the panel clamps against machine bounds shifted by
// gcode_move.homing_origin, because every position it shows and every value
// it types is gcode_move.gcode_position.
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture, "motion bounds follow the gcode origin, not the machine",
                 "[motion][coords][xml]") {
    std::array<lv_obj_t*, kUiPanelCount> panels{};
    for (auto& p : panels)
        p = lv_obj_create(lv_screen_active());
    NavigationManager::instance().set_panels(panels.data());

    auto& settings = helix::SettingsManager::instance();
    settings.init_subjects();
    settings.set_motion_show_actual_position(false);
    // Pin the jog distance the press dispatches so the clamp result is exact.
    settings.set_jog_distance(helix::JogMode::Coarse, /*outer=*/false, 1.0f);

    lv_obj_t* cached = nullptr;
    REQUIRE(get_global_motion_panel().show(lv_screen_active()));
    cached = get_global_motion_panel().get_root();
    helix::ui::UpdateQueue::instance().drain();

    MotionPanel& panel = get_global_motion_panel();
    lv_obj_t* root = panel.get_root();
    REQUIRE(root != nullptr);
    lv_obj_t* z_up_large = lv_obj_find_by_name(root, "z_up_large");
    lv_obj_t* z_up_small = lv_obj_find_by_name(root, "z_up_small");
    lv_obj_t* z_down = lv_obj_find_by_name(root, "z_down_large");
    REQUIRE(z_up_large != nullptr);
    REQUIRE(z_up_small != nullptr);
    REQUIRE(z_down != nullptr);

    // Machine envelope Z 0..275 with a G-code origin of 0.06 (the U1 numbers):
    // the valid gcode range is Z -0.06..274.94.
    get_printer_state().update_from_status({{"toolhead",
                                             {{"homed_axes", "xyz"},
                                              {"axis_minimum", {0.0, 0.0, 0.0, 0.0}},
                                              {"axis_maximum", {235.0, 235.0, 275.0, 0.0}}}},
                                            {"gcode_move",
                                             {{"homing_origin", {-0.0889, -0.016, 0.06, 0.0}},
                                              {"gcode_position", {10.0, 10.0, 274.94, 0.0}}}}});
    get_printer_state().set_klippy_state_sync(helix::KlippyState::READY);
    lv_subject_set_int(get_printer_state().print_state().get_print_state_enum_subject(),
                       static_cast<int>(helix::PrintJobState::STANDBY));
    helix::ui::UpdateQueue::instance().drain();

    // At the gcode ceiling the up buttons disable and down stays enabled.
    CHECK(lv_obj_has_state(z_up_large, LV_STATE_DISABLED));
    CHECK(lv_obj_has_state(z_up_small, LV_STATE_DISABLED));
    CHECK_FALSE(lv_obj_has_state(z_down, LV_STATE_DISABLED));

    // The keypad bounds the same envelope: Z tops out at 274.94, not 275.
    const auto params = helix::keypad_params_for_axis(
        get_printer_state().motion_state().get_gcode_axis_bounds(), helix::Axis::Z, 274.94);
    REQUIRE(params.has_value());
    CHECK(params->max_value == Catch::Approx(274.94f));
    CHECK(params->min_value == Catch::Approx(-0.06f));

    // A +1 jog from 274.5 clamps to the 0.44 left before the ceiling.
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPI api{client, get_printer_state()};
    IMoonrakerAPI* previous_api = get_moonraker_api();
    set_moonraker_api(&api);
    client.clear_gcode_script_history();

    get_printer_state().update_from_status(
        {{"gcode_move", {{"gcode_position", {10.0, 10.0, 274.5, 0.0}}}}});
    helix::ui::UpdateQueue::instance().drain();
    CHECK_FALSE(lv_obj_has_state(z_up_small, LV_STATE_DISABLED));

    helix::TextLogCapture log;
    lv_obj_send_event(z_up_small, LV_EVENT_PRESSED, nullptr);
    REQUIRE(panel.jog_hold_timer().poll(helix::HoldRepeat::DELAY_MS));
    const std::string blob = log.get_captured();
    CHECK(blob.find("Z jog: +0.42mm") != std::string::npos);

    lv_obj_send_event(z_up_small, LV_EVENT_RELEASED, nullptr);
    lv_obj_send_event(z_up_small, LV_EVENT_CLICKED, nullptr);

    // The dispatched relative move carries the clamped travel: the mock
    // records one history entry per gcode line, so join before matching.
    std::string all_scripts;
    for (const auto& script : client.gcode_script_history()) {
        all_scripts += script;
        all_scripts += "\n";
    }
    CHECK(all_scripts.find("G91") != std::string::npos);
    CHECK(all_scripts.find("Z0.42") != std::string::npos); // stops the edge margin short of 0.44
    CHECK(all_scripts.find("Z1") == std::string::npos);

    set_moonraker_api(previous_api);
    helix::ui::UpdateQueue::instance().drain();
    StaticPanelRegistry::instance().destroy_all();
    helix::ui::UpdateQueue::instance().drain();
}
