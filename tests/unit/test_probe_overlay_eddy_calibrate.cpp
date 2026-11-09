// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_probe_overlay_eddy_calibrate.cpp
 * @brief The Probe overlay's scan-probe calibrate buttons open the Z-offset
 *        calibration flow.
 *
 * PROBE_EDDY_CURRENT_CALIBRATE, BEACON_CALIBRATE and CARTOGRAPHER_SCAN_CALIBRATE
 * put Klipper in a manual probe session that needs jog, Accept and Abort
 * controls on the screen. Those live on the Z-offset panel, so each button
 * opens it rather than sending the command into a session nothing on the
 * screen can finish (#1994).
 */

#include "ui_nav_manager.h"
#include "ui_panel_calibration_zoffset.h"
#include "ui_probe_overlay.h"

#include "../lvgl_ui_test_fixture.h"
#include "app_globals.h"
#include "core/static_panel_registry.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "printer_discovery.h"
#include "probe_sensor_manager.h"

#include <array>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

namespace {

class EddyCalibrateFixture : public LVGLUITestFixture {
  protected:
    EddyCalibrateFixture() {
        for (auto& p : panels_)
            p = lv_obj_create(lv_screen_active());
        NavigationManager::instance().set_panels(panels_.data());
        ui_probe_overlay_register_callbacks();

        helix::PrinterDiscovery hw;
        hw.parse_objects(nlohmann::json::array({"probe_eddy_current btt_eddy"}));
        helix::sensors::ProbeSensorManager::instance().discover(hw);
    }

    ~EddyCalibrateFixture() override {
        helix::sensors::ProbeSensorManager::instance().discover(helix::PrinterDiscovery{});
        StaticPanelRegistry::instance().destroy_all();
        settle();
    }

    void settle() {
        for (int i = 0; i < 5; ++i) {
            helix::ui::UpdateQueue::instance().drain();
            process_lvgl(50);
        }
    }

    /// Click a button bound to @p callback and check the Z-offset panel opened
    /// in IDLE: the run starts from the panel's own Start.
    void expect_opens_zoffset_panel(const char* callback) {
        auto& panel = get_global_zoffset_cal_panel();
        REQUIRE(panel.get_root() == nullptr);

        lv_event_cb_t cb = lv_xml_get_event_cb(nullptr, callback);
        REQUIRE(cb != nullptr);
        lv_obj_t* button = lv_button_create(lv_screen_active());
        lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_send_event(button, LV_EVENT_CLICKED, nullptr);
        settle();

        REQUIRE(panel.get_root() != nullptr);
        CHECK(panel.is_visible());
        CHECK(panel.get_state() == ZOffsetCalibrationPanel::State::IDLE);

        NavigationManager::instance().go_back();
        settle();
    }

    std::array<lv_obj_t*, kUiPanelCount> panels_{};
};

} // namespace

TEST_CASE_METHOD(EddyCalibrateFixture, "Probe overlay: Eddy Calibrate opens the Z-offset panel",
                 "[probe][zoffset][eddy][1994]") {
    expect_opens_zoffset_panel("on_eddy_calibrate");
}

TEST_CASE_METHOD(EddyCalibrateFixture, "Probe overlay: Beacon Calibrate opens the Z-offset panel",
                 "[probe][zoffset][1994]") {
    expect_opens_zoffset_panel("on_beacon_calibrate");
}

TEST_CASE_METHOD(EddyCalibrateFixture,
                 "Probe overlay: Cartographer Scan Calibrate opens the Z-offset panel",
                 "[probe][zoffset][1994]") {
    expect_opens_zoffset_panel("on_carto_scan_cal");
}
