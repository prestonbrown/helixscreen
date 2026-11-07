// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_pa_calibration_panel.cpp
 * @brief The PA calibration screen across a screensaver suspend and wake
 *
 * Run with: ./build/bin/helix-tests "[pa_cal][1516]"
 *
 * The screen is opened through NavigationManager and blanked with
 * suspend_active() / resume_active(), the path the screensaver takes.
 */

#include "ui_nav_manager.h"
#include "ui_panel_calibration_pa.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "static_panel_registry.h"

#include <array>

#include "../catch_amalgamated.hpp"

using helix::ui::get_global_pa_cal_panel;
using helix::ui::PACalibrationPanel;

namespace {

class PACalPanelFixture : public LVGLUITestFixture {
  protected:
    PACalPanelFixture() {
        for (auto& p : panels_) {
            p = lv_obj_create(lv_screen_active());
        }
        NavigationManager::instance().set_panels(panels_.data());
        REQUIRE(panel().show(lv_screen_active()));
        settle();
    }

    ~PACalPanelFixture() override {
        NavigationManager::instance().resume_active();
        panel().set_state_for_test(PACalibrationPanel::IDLE);
        StaticPanelRegistry::instance().destroy_all();
        settle();
    }

    static PACalibrationPanel& panel() {
        return get_global_pa_cal_panel();
    }

    void settle() {
        for (int i = 0; i < 5; ++i) {
            helix::ui::UpdateQueue::instance().drain();
            process_lvgl(50);
        }
    }

    std::array<lv_obj_t*, kUiPanelCount> panels_{};
};

} // namespace

TEST_CASE_METHOD(PACalPanelFixture, "PA calibration: the screensaver does not stop a measurement",
                 "[pa_cal][1516]") {
    panel().set_state_for_test(PACalibrationPanel::MEASURING);

    NavigationManager::instance().suspend_active();
    settle();
    CHECK(panel().get_state() == PACalibrationPanel::MEASURING);

    NavigationManager::instance().resume_active();
    settle();
    CHECK(panel().get_state() == PACalibrationPanel::MEASURING);
}

TEST_CASE_METHOD(PACalPanelFixture,
                 "PA calibration: a heat-up pauses its clock behind the screensaver and resumes it",
                 "[pa_cal][1516]") {
    panel().set_state_for_test(PACalibrationPanel::HEATING);
    panel().arm_eta_timer_for_test();
    REQUIRE(panel().eta_timer_for_test() != nullptr);

    NavigationManager::instance().suspend_active();
    settle();
    CHECK(panel().get_state() == PACalibrationPanel::HEATING);
    CHECK(panel().eta_timer_for_test() == nullptr);

    NavigationManager::instance().resume_active();
    settle();
    CHECK(panel().get_state() == PACalibrationPanel::HEATING);
    CHECK(panel().eta_timer_for_test() != nullptr);
}

TEST_CASE_METHOD(PACalPanelFixture, "PA calibration: a result is still shown on wake",
                 "[pa_cal][1516]") {
    panel().set_state_for_test(PACalibrationPanel::MEASURING);
    NavigationManager::instance().suspend_active();
    panel().on_result_for_test(0.042f);
    settle();

    NavigationManager::instance().resume_active();
    settle();
    CHECK(panel().get_state() == PACalibrationPanel::COMPLETE);
    CHECK(panel().last_result() == Catch::Approx(0.042f));
}
