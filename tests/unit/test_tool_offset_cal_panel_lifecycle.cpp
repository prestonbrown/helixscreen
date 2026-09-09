// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_tool_offset_cal_panel_lifecycle.cpp
 * @brief A calibration run outlives the screen it was started from.
 *
 * The macro blocks Klipper for minutes and the only stop is M112, so pressing
 * Back must not end the run - and must not lose its completion either.
 * OverlayBase expires lifetime_ on every on_deactivate(); the run's callbacks
 * ride on a guard that Stop, cleanup() and destruction expire instead.
 */

#include "ui_panel_calibration_tool_offset.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/scoped_env.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "tool_offset_calibration.h"
#include "tool_state.h"

#include <cstdlib>
#include <functional>
#include <optional>

#include "../catch_amalgamated.hpp"

using nlohmann::json;
namespace cal = helix::tool_offset_calibration;

namespace {

struct ToolCalPanelFixture : public LVGLTestFixture {
    /// The toolchanger persona is chosen by HELIX_MOCK_AMS when the mock is
    /// constructed (see test_mock_tool_offset_calibration.cpp).
    helix::ScopedEnv ams_env{"HELIX_MOCK_AMS"};
    std::optional<MoonrakerClientMock> client;
    std::optional<MoonrakerAPI> api;

    ToolCalPanelFixture() {
        setenv("HELIX_MOCK_AMS", "toolchanger", 1);
        client.emplace(MoonrakerClientMock::PrinterType::VORON_24, 100.0);

        // begin_run() asks the global PrinterState whether the printer supports
        // the macro, and the API gates execute_gcode() on klippy being READY.
        helix::PrinterState& ps = get_printer_state();
        helix::PrinterStateTestAccess::reset(ps);
        ps.init_subjects(false);
        ps.set_klippy_state_sync(helix::KlippyState::READY);
        helix::PrinterDiscovery hw;
        hw.parse_objects(
            json::array({"gcode_move", "toolhead", "extruder", "toolchanger", "tool T0", "tool T1",
                         "tool T2", "tool T3", "gcode_macro CALIBRATE_TOOL_OFFSETS"}));
        ps.set_hardware(hw);

        helix::ToolState& ts = helix::ToolState::instance();
        ts.deinit_subjects();
        ts.init_subjects(false);
        ts.init_tools(hw);

        api.emplace(*client, ps);
        set_moonraker_api(&*api);
    }
    ~ToolCalPanelFixture() override {
        helix::ui::UpdateQueue::instance().drain();
        set_moonraker_api(nullptr);
    }

    /// Pumps the mock's calibration timer (600 ms per tick), LVGL, and the
    /// UpdateQueue the run's bg_cb callbacks land on.
    bool pump_until(const std::function<bool()>& done, int max_ticks = 200) {
        for (int i = 0; i < max_ticks && !done(); ++i) {
            lv_tick_inc(100);
            lv_timer_handler_safe();
            helix::ui::UpdateQueue::instance().drain();
        }
        return done();
    }
};

} // namespace

TEST_CASE_METHOD(ToolCalPanelFixture, "tool offset panel: a run finishes after Back",
                 "[ui_integration][toolchanger][tool_offset_cal]") {
    helix::ui::ToolOffsetCalibrationPanel panel;
    panel.init_subjects();
    panel.on_activate();

    panel.begin_run();
    REQUIRE(panel.is_calibration_active());
    REQUIRE(lv_subject_get_int(panel.get_active_subject()) == 1);

    // Back, mid-run. The base expires lifetime_ here.
    panel.on_deactivate();
    REQUIRE(panel.is_calibration_active());

    // The macro runs to completion on the printer while the panel is hidden,
    // and its completion still lands: nothing is left reading "active".
    REQUIRE(pump_until([&] { return !panel.is_calibration_active(); }));
    CHECK(lv_subject_get_int(panel.get_active_subject()) == 0);
    CHECK_FALSE(panel.run().failed());
    CHECK(panel.run().step(1) == cal::ToolStep::Done);
    CHECK(panel.run().step(3) == cal::ToolStep::Done);

    panel.on_activate(); // back on screen: the rows repaint from that state
    panel.on_deactivate();
    panel.cleanup();
}

TEST_CASE_METHOD(ToolCalPanelFixture, "tool offset panel: Stop drops the run's callbacks",
                 "[ui_integration][toolchanger][tool_offset_cal]") {
    // Stop is M112 + FIRMWARE_RESTART; the rpc then fails with the shutdown,
    // and that failure must not be reported as the run's own.
    helix::ui::ToolOffsetCalibrationPanel panel;
    panel.init_subjects();
    panel.on_activate();

    panel.begin_run();
    REQUIRE(panel.is_calibration_active());
    REQUIRE(panel.abort_in_progress_calibration());
    CHECK_FALSE(panel.is_calibration_active());
    CHECK(std::string(lv_subject_get_string(panel.get_status_subject())) == "Stopped");

    pump_until([] { return false; }, 120);
    CHECK(std::string(lv_subject_get_string(panel.get_status_subject())) == "Stopped");
    CHECK_FALSE(panel.run().failed());

    panel.on_deactivate();
    panel.cleanup();
}
