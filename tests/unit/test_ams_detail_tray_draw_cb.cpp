// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_detail_tray_draw_cb.cpp
 * @brief Regression tests for the 3D tray draw-callback attachment in
 *        ams_detail_update_tray()
 *
 * ams_detail_update_tray() runs on every panel rebuild, so repeated calls must
 * not accumulate draw callbacks; idempotence comes from lv_obj_remove_event_cb()
 * ahead of lv_obj_add_event_cb().
 *
 * tray_back_draw_cb / tray_front_draw_cb have internal linkage in
 * src/ui/filament/ui_ams_detail.cpp, so the test cannot name them. Instead the test builds
 * the grid and tray from bare lv_obj_create() containers, which start with zero
 * event descriptors, then asserts that exactly one descriptor exists after the
 * first call and that it is still the same single descriptor - identical callback
 * pointer - after the second.
 */

#include "ui_ams_detail.h"

#include "../lvgl_test_fixture.h"
#include "ams_state.h"

#include "../catch_amalgamated.hpp"

/// LVGL plus a backend-free AmsState.
///
/// ams_detail_update_tray() hides the tray outright for a backend that reports
/// has_physical_tray() == false (tool changers). Clearing backends makes
/// get_backend(0) return nullptr so the physical-tray path is taken regardless of
/// what an earlier test in the binary left registered.
class AmsDetailTrayFixture : public LVGLTestFixture {
  public:
    AmsDetailTrayFixture() {
        helix::AmsState::instance().clear_backends();
    }
    ~AmsDetailTrayFixture() override {
        helix::AmsState::instance().clear_backends();
    }
};

namespace {

/// Build the minimum widget pair ams_detail_update_tray() operates on.
///
/// The function early-returns unless slot_grid reports a positive height, so the
/// grid gets an explicit size plus a layout pass (a freshly created object reports
/// LVGL's default until the computed coords refresh).
AmsDetailWidgets make_tray_widgets(lv_obj_t* parent) {
    AmsDetailWidgets w;
    w.root = lv_obj_create(parent);
    lv_obj_set_size(w.root, 480, 272);

    w.slot_grid = lv_obj_create(w.root);
    lv_obj_set_size(w.slot_grid, 400, 200);

    w.slot_tray = lv_obj_create(w.root);
    lv_obj_set_width(w.slot_tray, 400);

    lv_obj_update_layout(w.root);
    return w;
}

/// Callback pointer of the descriptor at @p index, or nullptr when out of range.
lv_event_cb_t event_cb_at(lv_obj_t* obj, uint32_t index) {
    if (index >= lv_obj_get_event_count(obj))
        return nullptr;
    lv_event_dsc_t* dsc = lv_obj_get_event_dsc(obj, index);
    return dsc ? lv_event_dsc_get_cb(dsc) : nullptr;
}

} // namespace

TEST_CASE_METHOD(AmsDetailTrayFixture,
                 "ams_detail_update_tray attaches draw callbacks idempotently",
                 "[ams][ui][user_flags]") {
    AmsDetailWidgets w = make_tray_widgets(test_screen());

    REQUIRE(lv_obj_get_event_count(w.slot_grid) == 0);
    REQUIRE(lv_obj_get_event_count(w.slot_tray) == 0);

    ams_detail_update_tray(w);

    // One back-wall callback on the grid, one front-face callback on the tray.
    REQUIRE(lv_obj_get_event_count(w.slot_grid) == 1);
    REQUIRE(lv_obj_get_event_count(w.slot_tray) == 1);

    lv_event_cb_t back_cb = event_cb_at(w.slot_grid, 0);
    lv_event_cb_t front_cb = event_cb_at(w.slot_tray, 0);
    REQUIRE(back_cb != nullptr);
    REQUIRE(front_cb != nullptr);

    // Second and third passes model a panel rebuild. Without the remove-then-add
    // the counts climb and every extra registration redraws the tray again.
    ams_detail_update_tray(w);
    ams_detail_update_tray(w);

    CHECK(lv_obj_get_event_count(w.slot_grid) == 1);
    CHECK(lv_obj_get_event_count(w.slot_tray) == 1);

    // Same callback still installed, not a different one that happens to keep the
    // count at 1.
    CHECK(event_cb_at(w.slot_grid, 0) == back_cb);
    CHECK(event_cb_at(w.slot_tray, 0) == front_cb);

    lv_obj_delete(w.root);
}
