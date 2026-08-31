// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_touch_calibration_rotation.cpp
 * @brief The evdev range fit must not run on a rotated display
 *
 * The 3-point solve maps raw -> LOGICAL screen (the wizard's targets are
 * logical, post-rotation), but the solved range executes at the evdev stage
 * whose output LVGL rotates again — on a rotated panel the rotation folds
 * into the stored (min,max,swap) and every later tap lands through a double
 * transform, with the evdev clamp flattening the logical-shaped output
 * against the physical extents (prestonbrown/helixscreen#1394). Rotated
 * panels keep the affine-only shape, the pre-range-stage path, which composes
 * correctly with lv_display_rotate_point().
 */

#include "../lvgl_test_fixture.h"
#include "touch_calibration.h"
#include "touch_calibration_panel.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Drive the panel through a full capture with pre-scale digitizer readings
/// that are AFFINE-CONSISTENT with the panel's own crosshair targets: the
/// range fit solves device_raw vs screen_points_, and screen_points_ are the
/// TARGETS (compute_target_position), not the tap coordinates. A true raw
/// axis of 2.5 units per pixel on both axes, no swap, reproduces the targets
/// exactly, so the implied ABS range (0..2000 both axes) is plausible and the
/// fit computes wherever the gate allows it.
void capture_three_points_with_raw(TouchCalibrationPanel& panel) {
    // compute_target_position() on 800x480: ratios (0.15,0.20) (0.50,0.78)
    // (0.85,0.20) -> (120,96) (400,374) (680,96). Hardcoded so the derivation
    // is visible; if the ratios change, this test's raws must move with them.
    const Point targets[3] = {{120, 96}, {400, 374}, {680, 96}};
    Point raws[3];
    for (int i = 0; i < 3; i++) {
        raws[i] = Point{targets[i].x * 5 / 2, targets[i].y * 5 / 2};
    }

    for (int i = 0; i < 3; i++) {
        panel.capture_point(raws[i], &raws[i]);
    }
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "range fit stays off when the display is rotated",
                 "[touch-calibration][rotation][1394]") {
    TouchCalibrationPanel panel;
    panel.set_screen_size(800, 480);

    lv_display_set_rotation(lv_display_get_default(), LV_DISPLAY_ROTATION_90);
    panel.start();
    capture_three_points_with_raw(panel);

    REQUIRE(panel.get_state() == TouchCalibrationPanel::State::VERIFY);
    // The whole point of the gate: no evdev range on a rotated panel.
    CHECK_FALSE(panel.get_range_fit().valid);
    // The affine half is unaffected — rotated panels still calibrate.
    REQUIRE(panel.get_calibration() != nullptr);
    CHECK(panel.get_calibration()->valid);
}

TEST_CASE_METHOD(LVGLTestFixture, "range fit solves on an unrotated display",
                 "[touch-calibration][rotation][1394]") {
    TouchCalibrationPanel panel;
    panel.set_screen_size(800, 480);

    lv_display_set_rotation(lv_display_get_default(), LV_DISPLAY_ROTATION_0);
    panel.start();
    capture_three_points_with_raw(panel);

    REQUIRE(panel.get_state() == TouchCalibrationPanel::State::VERIFY);
    // The same capture with raw readings produces a range fit when nothing
    // rotates underneath it.
    CHECK(panel.get_range_fit().valid);
    CHECK(panel.get_calibration()->valid);
}
