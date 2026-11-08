// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
// Full-screen touch-capture hit-testing for the Settings recalibrate overlay
// (TouchCalibrationOverlay). During active point capture the affine transform is
// disabled so raw points can be captured — LVGL then sees uncalibrated (offset)
// coordinates. On panels that actually need calibration (e.g. Qidi Q2, 480x272,
// Goodix) an uncalibrated tap on a top-edge target reports a coordinate that
// drifts UP into the header strip, where the full-width, clickable header Back
// button lives (its hit area is further widened by ext_click_area = header/2).
//
// The panel lifts the invisible touch_capture_overlay onto the active screen at
// 100%x100% so it covers the ENTIRE screen — header included. This test asserts
// that during POINT_1 capture no screen location (header strip or target) routes
// to back_button, and that the corner Cancel chip is hittable and clear of every
// target. The header strip must not resolve to back_button (the Q2
// recalibrate back-leak).

#include "ui_component_header_bar.h"
#include "ui_touch_calibration_overlay.h"

#include "../test_fixtures.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "touch_calibration_panel.h"

#include <string>

#include "../catch_amalgamated.hpp"

namespace {

// Micro-tier geometry that reproduces the Q2: 40px header (ui_get_responsive_
// header_height for <400px), targets at 20%/78% of height.
constexpr int32_t kMicroW = 480;
constexpr int32_t kMicroH = 272;

lv_obj_t* hit_at(lv_obj_t* screen, int32_t x, int32_t y) {
    lv_point_t p{x, y};
    return lv_indev_search_obj(screen, &p);
}

std::string name_of(lv_obj_t* obj) {
    if (obj == nullptr)
        return "<null>";
    const char* n = lv_obj_get_name(obj);
    return n != nullptr ? n : "<unnamed>";
}

// Is `node` the same object as `ancestor`, or a descendant of it?
bool is_within(lv_obj_t* node, lv_obj_t* ancestor) {
    for (lv_obj_t* o = node; o != nullptr; o = lv_obj_get_parent(o)) {
        if (o == ancestor)
            return true;
    }
    return false;
}

bool rect_contains(const lv_area_t& a, int32_t x, int32_t y) {
    return x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2;
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture,
                 "Touch recalibrate: full-screen capture surface, no header Back leak",
                 "[touch][hittest][calibration]") {
    // --- Register the overlay + its XML dependencies ---------------------------
    REQUIRE(register_component("header_bar"));
    REQUIRE(register_component("overlay_panel"));
    REQUIRE(register_component("touch_calibration_overlay"));

    // --- Stand up an isolated 480x272 (micro) display --------------------------
    // A real micro display makes ui_component_header_bar_setup() resolve the 40px
    // header + ext_click_area=20 that pulls the Back button's hit area down over
    // the top-row targets — the exact device condition. Restored/deleted at end.
    lv_display_t* prev_display = lv_display_get_default();
    lv_display_t* disp = lv_display_create(kMicroW, kMicroH);
    REQUIRE(disp != nullptr);
    lv_display_set_default(disp);
    lv_obj_t* screen = lv_obj_create(nullptr);
    lv_screen_load(screen);

    struct DisplayGuard {
        lv_display_t* disp;
        lv_display_t* prev;
        ~DisplayGuard() {
            lv_display_set_default(prev);
            lv_display_delete(disp); // also deletes its screens + reparented widgets
        }
    } guard{disp, prev_display};

    // --- Build the overlay -----------------------------------------------------
    helix::ui::TouchCalibrationOverlay overlay;
    overlay.init_subjects();
    overlay.register_callbacks();

    lv_obj_t* root = overlay.create(screen);
    REQUIRE(root != nullptr);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_HIDDEN);

    // Force full-screen geometry (the runtime overlay_width_transient token is not
    // registered in the unit-test XML scope; pin it deterministically).
    lv_obj_set_align(root, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_size(root, kMicroW, kMicroH);

    // Apply the real header setup: 40px height + ext_click_area on back_button.
    lv_obj_t* header = lv_obj_find_by_name(root, "overlay_header");
    REQUIRE(header != nullptr);
    ui_component_header_bar_setup(header, screen);

    // --- Drive the panel into POINT_1 capture ----------------------------------
    helix::TouchCalibrationPanel* panel = overlay.get_panel();
    REQUIRE(panel != nullptr);
    panel->set_screen_size(kMicroW, kMicroH);
    panel->start(); // -> POINT_1

    // POINT_1 state: shows crosshair + capture surface + Cancel chip.
    lv_subject_t* state = lv_xml_get_subject(nullptr, "touch_cal_state");
    REQUIRE(state != nullptr);
    lv_subject_set_int(state, 1); // kStatePoint1

    lv_obj_update_layout(screen);

    // Lifts crosshair + capture surface full-screen and raises the chip.
    overlay.on_activate();
    lv_obj_update_layout(screen);

    // --- Resolve the widgets we assert against ---------------------------------
    lv_obj_t* capture = lv_obj_find_by_name(screen, "touch_capture_overlay");
    lv_obj_t* back_btn = lv_obj_find_by_name(screen, "back_button");
    lv_obj_t* chip = lv_obj_find_by_name(screen, "cancel_chip");
    REQUIRE(capture != nullptr);
    REQUIRE(back_btn != nullptr);
    REQUIRE(chip != nullptr);

    const helix::Point t0 = panel->get_target_position(0); // top-left
    const helix::Point t1 = panel->get_target_position(1); // bottom-center
    const helix::Point t2 = panel->get_target_position(2); // top-right

    // --- LEAK: header-strip points above the top targets -----------------------
    // These land where an uncalibrated top-target tap drifts to (y inside the 40px
    // header). They must resolve to the capture surface, not back_button. This is
    // the load-bearing regression assertion.
    {
        lv_obj_t* got = hit_at(screen, t0.x, 10);
        INFO("header strip above target-0 (" << t0.x << ",10) -> " << name_of(got));
        CHECK(got != back_btn);
        CHECK(got == capture);
    }
    {
        lv_obj_t* got = hit_at(screen, t2.x, 10);
        INFO("header strip above target-2 (" << t2.x << ",10) -> " << name_of(got));
        CHECK(got != back_btn);
        CHECK(got == capture);
    }

    // --- Every calibration target routes to the capture surface, never Back -----
    for (const auto& t : {t0, t1, t2}) {
        lv_obj_t* got = hit_at(screen, t.x, t.y);
        INFO("target (" << t.x << "," << t.y << ") -> " << name_of(got));
        CHECK(got != back_btn);
        CHECK(got == capture);
    }

    // --- Cancel chip: hittable, and clear of every target ----------------------
    {
        lv_area_t chip_area;
        lv_obj_get_coords(chip, &chip_area);
        int32_t cx = (chip_area.x1 + chip_area.x2) / 2;
        int32_t cy = (chip_area.y1 + chip_area.y2) / 2;

        lv_obj_t* got = hit_at(screen, cx, cy);
        INFO("cancel chip centre (" << cx << "," << cy << ") -> " << name_of(got));
        CHECK(is_within(got, chip));

        // The chip must not sit under any calibration target.
        CHECK_FALSE(rect_contains(chip_area, t0.x, t0.y));
        CHECK_FALSE(rect_contains(chip_area, t1.x, t1.y));
        CHECK_FALSE(rect_contains(chip_area, t2.x, t2.y));
    }

    // --- Cancel chip visibility is exactly states 1-3 --------------------------
    // The chip must show ONLY during active capture (POINT_1..POINT_3) and hide in
    // IDLE(0)/VERIFY(4)/COMPLETE(5) where the normal Back chrome returns. A single
    // bind_flag_if drives this; three separate bind_flag_if_eq on the same `hidden`
    // flag would fight (each clears on non-match, last-registered wins) and leave
    // the chip visible in IDLE/VERIFY.
    struct StateVis {
        int32_t st;
        bool hidden;
    };
    for (const auto& sv : {StateVis{0, true}, StateVis{1, false}, StateVis{2, false},
                           StateVis{3, false}, StateVis{4, true}, StateVis{5, true}}) {
        lv_subject_set_int(state, sv.st);
        lv_obj_update_layout(screen);
        INFO("cancel chip in state " << sv.st << " hidden=expected " << sv.hidden);
        CHECK(lv_obj_has_flag(chip, LV_OBJ_FLAG_HIDDEN) == sv.hidden);
    }
    // --- VERIFY: capture surface is hidden and Accept is reachable -------------
    // In VERIFY (state 4) the full-screen capture surface must hide so its taps
    // stop grabbing — otherwise, foreground-lifted onto the screen root, it covers
    // the Accept/Retry buttons and the calibration is unwinnable. Two stacked
    // bind_flag_if_eq on the capture surface's `hidden` flag (ref_value 4 and 5)
    // fight — each clears on non-match, last-registered wins — leaving it VISIBLE
    // in VERIFY. A single bind_flag_if must drive it. Regression: Accept
    // unclickable after #1029 lifted the capture surface to screen foreground.
    {
        lv_subject_set_int(state, 4); // kStateVerify
        lv_obj_update_layout(screen);
        INFO("capture surface in VERIFY hidden=" << lv_obj_has_flag(capture, LV_OBJ_FLAG_HIDDEN));
        CHECK(lv_obj_has_flag(capture, LV_OBJ_FLAG_HIDDEN));

        lv_obj_t* accept = lv_obj_find_by_name(screen, "btn_accept");
        REQUIRE(accept != nullptr);
        lv_area_t a;
        lv_obj_get_coords(accept, &a);
        int32_t ax = (a.x1 + a.x2) / 2;
        int32_t ay = (a.y1 + a.y2) / 2;
        lv_obj_t* got = hit_at(screen, ax, ay);
        INFO("Accept centre (" << ax << "," << ay << ") -> " << name_of(got));
        CHECK(got != capture);
        CHECK(is_within(got, accept));
    }

    lv_subject_set_int(state, 1); // restore capture state before teardown

    // --- Teardown restores the reparented widgets (no orphan on the screen) -----
    overlay.on_deactivate(DeactivateReason::NavigateAway);
    lv_obj_update_layout(screen);
    // After dismiss the capture surface is back inside the overlay subtree, so a
    // header-strip tap is owned by the Back button again (normal chrome returns).
    {
        lv_obj_t* got = hit_at(screen, t0.x, 10);
        INFO("after deactivate, header strip -> " << name_of(got));
        CHECK(lv_obj_get_parent(capture) != screen);
    }

    overlay.cleanup();
}
