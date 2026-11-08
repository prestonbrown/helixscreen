// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Regression: TouchCalibrationPanel's timer starters must cancel before they
// create.
//
// start_fast_revert_timer() and start_countdown_timer() must cancel before
// assigning over their handle. Otherwise re-entering VERIFY leaves the previous
// lv_timer_t armed in LVGL's global list with user_data == the panel, the
// destructor can only delete the newest handle, and the orphan fires inside
// some later, unrelated test's process_lvgl() and writes through a freed `this`
// (AddressSanitizer: heap-buffer-overflow WRITE of 8 bytes at
// touch_calibration_panel.cpp `self->fast_revert_timer_ = nullptr`).
//
// start_stall_timer() has the same cancel-first shape; these pin the other two
// to it. The leak is invisible without counting LVGL's timers, because the
// panel's own handle looks perfectly healthy either way.

#include "../lvgl_test_fixture.h"
#include "../test_helpers/touch_calibration_panel_test_access.h"
#include "touch_calibration_panel.h"

#include "../catch_amalgamated.hpp"

using helix::TouchCalibrationPanel;
using helix::TouchCalibrationPanelTestAccess;

namespace {

/// Timers currently registered with LVGL, whoever owns them.
int live_timer_count() {
    int n = 0;
    for (lv_timer_t* t = lv_timer_get_next(nullptr); t != nullptr; t = lv_timer_get_next(t)) {
        ++n;
    }
    return n;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "touch cal: restarting the fast-revert timer strands nothing",
                 "[touch-calibration][timer][crash_hardening]") {
    TouchCalibrationPanel panel;

    const int baseline = live_timer_count();

    TouchCalibrationPanelTestAccess::start_fast_revert_timer(panel);
    lv_timer_t* first = TouchCalibrationPanelTestAccess::fast_revert_timer(panel);
    REQUIRE(first != nullptr);
    REQUIRE(live_timer_count() == baseline + 1);

    // Second start (production reaches this by re-entering VERIFY).
    TouchCalibrationPanelTestAccess::start_fast_revert_timer(panel);
    lv_timer_t* second = TouchCalibrationPanelTestAccess::fast_revert_timer(panel);
    REQUIRE(second != nullptr);

    // The panel's handle always looks fine; the leak is the count. Assign-over
    // leaves baseline + 2, one of them unreachable from the panel forever.
    REQUIRE(live_timer_count() == baseline + 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "touch cal: restarting the countdown timer strands nothing",
                 "[touch-calibration][timer][crash_hardening]") {
    TouchCalibrationPanel panel;

    const int baseline = live_timer_count();

    TouchCalibrationPanelTestAccess::start_countdown_timer(panel);
    REQUIRE(TouchCalibrationPanelTestAccess::countdown_timer(panel) != nullptr);
    REQUIRE(live_timer_count() == baseline + 1);

    TouchCalibrationPanelTestAccess::start_countdown_timer(panel);
    REQUIRE(TouchCalibrationPanelTestAccess::countdown_timer(panel) != nullptr);
    REQUIRE(live_timer_count() == baseline + 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "touch cal: destroying the panel leaves no timer behind",
                 "[touch-calibration][timer][crash_hardening]") {
    const int baseline = live_timer_count();
    {
        TouchCalibrationPanel panel;
        TouchCalibrationPanelTestAccess::start_fast_revert_timer(panel);
        TouchCalibrationPanelTestAccess::start_countdown_timer(panel);
        // Restart both, which is what produced the orphans.
        TouchCalibrationPanelTestAccess::start_fast_revert_timer(panel);
        TouchCalibrationPanelTestAccess::start_countdown_timer(panel);
        REQUIRE(live_timer_count() == baseline + 2);
    }
    // Every timer the panel created must die with it. Anything left over holds
    // user_data pointing at the destroyed panel and detonates on the next pump.
    REQUIRE(live_timer_count() == baseline);
}
