// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_screws_tilt_panel_suspend.cpp
 * @brief ScrewsTiltPanel keeps its results across a screensaver suspend
 *
 * Run with: ./build/bin/helix-tests "[screws_tilt][suspend]"
 */

#include "ui_panel_screws_tilt.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "calibration_types.h"

#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

std::vector<ScrewTiltResult> two_screws() {
    ScrewTiltResult ref;
    ref.screw_name = "front_left";
    ref.adjustment = "CW 00:00";
    ref.is_reference = true;
    ScrewTiltResult other;
    other.screw_name = "front_right";
    other.adjustment = "CW 00:45";
    return {ref, other};
}

int screw_row_visible(size_t index) {
    const std::string name = "screw_" + std::to_string(index) + "_visible";
    return lv_subject_get_int(lv_xml_get_subject(nullptr, name.c_str()));
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "screws tilt results survive the screensaver",
                 "[screws_tilt][suspend]") {
    ScrewsTiltPanel panel;
    panel.init_subjects();
    panel.on_activate();

    SECTION("results on screen before the suspend") {
        panel.on_screws_tilt_results(two_screws());
        REQUIRE(panel.get_state() == ScrewsTiltPanel::State::RESULTS);
        REQUIRE(screw_row_visible(1) == 1);
        panel.on_deactivate(DeactivateReason::Suspended);
    }
    SECTION("results landing behind the screensaver") {
        panel.on_deactivate(DeactivateReason::Suspended);
        panel.on_screws_tilt_results(two_screws());
    }

    panel.on_activate();
    helix::ui::UpdateQueue::instance().drain();

    CHECK(panel.get_state() == ScrewsTiltPanel::State::RESULTS);
    CHECK(screw_row_visible(0) == 1);
    CHECK(screw_row_visible(1) == 1);

    panel.on_deactivate(DeactivateReason::NavigateAway);
    helix::ui::UpdateQueue::instance().drain();
}
