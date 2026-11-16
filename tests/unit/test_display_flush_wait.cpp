// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_display_flush_wait.cpp
 * @brief No display in this binary may block a refresh (prestonbrown/helixscreen#1524)
 *
 * lv_refr.c waits for the previous flush to finish before it renders the next
 * area, and with no flush_wait_cb that wait is `while(disp->flushing);`, a bare
 * spin with no timeout and no yield. draw_buf_flush() raises that flag on every
 * flush but only lowers it by calling the display's flush callback, so a display
 * created without one latches it for the life of the process and the next
 * lv_refr_now() on that display never returns.
 *
 * Test displays render into a buffer nobody reads, so a flush is finished the
 * moment it is issued and a no-op flush_wait_cb is the honest answer for all of
 * them. HelixTestFixture::reset_all() installs one on every display, which is
 * what this file holds the binary to.
 */

// TEST_MIRROR_OK: the subject is the test binary's own display state and LVGL's
// refresh contract, so there is no include/ or src/ header to reach for.

#include "ui_test_utils.h"

#include "../test_fixtures.h"
#include "display/lv_display_private.h" // flush_wait_cb / flushing are private state
#include "lvgl/lvgl.h"

#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

/// Every display registered in this process, in LVGL's own order.
std::vector<lv_display_t*> all_displays() {
    std::vector<lv_display_t*> out;
    for (lv_display_t* d = lv_display_get_next(nullptr); d != nullptr; d = lv_display_get_next(d)) {
        out.push_back(d);
    }
    return out;
}

} // namespace

// The guarantee itself. Any display reachable here can be handed to lv_refr_now()
// by production code (DisplayManager's wake path does) or by a test helper, and
// without a flush_wait_cb that call is a coin flip on whether the process
// survives it.
TEST_CASE_METHOD(LVGLTestFixture, "every display can finish a flush wait",
                 "[lvgl][display][flush][1524]") {
    const auto displays = all_displays();
    REQUIRE_FALSE(displays.empty());

    for (size_t i = 0; i < displays.size(); ++i) {
        lv_display_t* d = displays[i];
        INFO("display " << i << " of " << displays.size() << ": "
                        << lv_display_get_horizontal_resolution(d) << "x"
                        << lv_display_get_vertical_resolution(d));
        CHECK(d->flush_wait_cb != nullptr);
    }
}
