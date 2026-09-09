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
#include "moonraker_error.h"
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
    // One row per tool the printer has - no fixed cap, the pools grew to fit.
    REQUIRE(lv_subject_get_int(panel.get_tool_count_subject()) == 4);
    REQUIRE(panel.get_row_state_subject(3) != nullptr);

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

TEST_CASE_METHOD(ToolCalPanelFixture,
                 "tool offset panel: an rpc timeout under a busy printer waits for the idle edge",
                 "[ui_integration][toolchanger][tool_offset_cal]") {
    // Moonraker never times out printer.gcode.script; our ceiling firing while
    // Klipper still reports idle_timeout "Printing" means the macro is still
    // running. Failing the run there re-enabled Save under a blocked queue.
    helix::PrinterState& ps = get_printer_state();
    helix::ui::ToolOffsetCalibrationPanel panel;
    panel.init_subjects();
    panel.on_activate();
    panel.begin_run();
    REQUIRE(panel.is_calibration_active());

    ps.update_from_status(json{{"idle_timeout", json{{"state", "Printing"}}}});
    helix::ui::UpdateQueue::instance().drain();
    panel.on_run_rpc_error(MoonrakerError::timeout("printer.gcode.script", 1));
    helix::ui::UpdateQueue::instance().drain();

    // Still a run: Save stays disabled, and the status says why.
    CHECK(panel.is_calibration_active());
    CHECK(lv_subject_get_int(panel.get_active_subject()) == 1);
    CHECK(std::string(lv_subject_get_string(panel.get_status_subject())) ==
          "Calibration may still be running — response timed out");

    // The printer going idle is the completion.
    ps.update_from_status(json{{"idle_timeout", json{{"state", "Ready"}}}});
    for (int pass = 0; pass < 4; ++pass) {
        helix::ui::UpdateQueue::instance().drain();
    }
    CHECK_FALSE(panel.is_calibration_active());
    CHECK(lv_subject_get_int(panel.get_active_subject()) == 0);
    CHECK_FALSE(panel.run().failed());
    CHECK(panel.run().step(2) == cal::ToolStep::Done);

    panel.on_deactivate();
    panel.cleanup();
}

TEST_CASE_METHOD(ToolCalPanelFixture,
                 "tool offset panel: an rpc timeout under an idle printer fails",
                 "[ui_integration][toolchanger][tool_offset_cal]") {
    // No macro running behind the silence: nothing to wait for.
    helix::PrinterState& ps = get_printer_state();
    ps.update_from_status(json{{"idle_timeout", json{{"state", "Ready"}}}});
    helix::ui::UpdateQueue::instance().drain();

    helix::ui::ToolOffsetCalibrationPanel panel;
    panel.init_subjects();
    panel.on_activate();
    panel.begin_run();
    REQUIRE(panel.is_calibration_active());

    panel.on_run_rpc_error(MoonrakerError::timeout("printer.gcode.script", 1));
    helix::ui::UpdateQueue::instance().drain();
    CHECK_FALSE(panel.is_calibration_active());
    CHECK(panel.run().failed());

    panel.on_deactivate();
    panel.cleanup();
}

TEST_CASE_METHOD(ToolCalPanelFixture, "tool offset panel: no tools is a refusal, not a run",
                 "[ui_integration][toolchanger][tool_offset_cal]") {
    // ToolState is empty between an AMS topology clear and the next
    // init_tools(). Run::begin(0) stays inactive, so starting anyway left the
    // panel reading active with a Stop that did nothing.
    helix::ToolState::instance().clear_ams_topology();
    REQUIRE(helix::ToolState::instance().tools().empty());

    helix::ui::ToolOffsetCalibrationPanel panel;
    panel.init_subjects();
    panel.on_activate();
    panel.begin_run();
    helix::ui::UpdateQueue::instance().drain();

    CHECK_FALSE(panel.is_calibration_active());
    CHECK(lv_subject_get_int(panel.get_active_subject()) == 0);

    // And coming back to the panel never leaves the subject ahead of the run.
    panel.on_deactivate();
    panel.on_activate();
    CHECK(lv_subject_get_int(panel.get_active_subject()) == 0);
    panel.on_deactivate();
    panel.cleanup();
}

TEST_CASE_METHOD(ToolCalPanelFixture, "tool offset panel: Save commits a pending babystep too",
                 "[ui_integration][toolchanger][tool_offset_cal]") {
    // The SAVE_CONFIG a tool save ends in restarts Klipper, which resets
    // homing_origin: a babystep not applied in that same restart is lost, while
    // its pending delta went on being shown. Save must apply it, as the header
    // and Controls saves do.
    helix::PrinterState& ps = get_printer_state();
    helix::PrinterStateTestAccess::pin_z_offset_strategy(
        ps, helix::ZOffsetCalibrationStrategy::PROBE_CALIBRATE);
    ps.update_from_status(
        json{{"gcode_move", json{{"homing_origin", json::array({0.0, 0.0, 0.05, 0.0})}}}});
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(lv_subject_get_int(ps.get_gcode_z_offset_subject()) == 50);

    helix::ui::ToolOffsetCalibrationPanel panel;
    panel.init_subjects();
    panel.on_activate();
    client->clear_gcode_script_history();
    panel.send_save();
    // The apply's success callback sends SAVE_CONFIG; pump until both are out.
    REQUIRE(pump_until([&] { return client->gcode_script_history().size() >= 2; }, 50));

    const auto& hist = client->gcode_script_history();
    CHECK(hist[hist.size() - 2] == "Z_OFFSET_APPLY_PROBE");
    CHECK(hist.back() == "SAVE_CONFIG");

    panel.on_deactivate();
    panel.cleanup();
}

TEST_CASE_METHOD(ToolCalPanelFixture,
                 "tool offset panel: Save applies no babystep when none is pending",
                 "[ui_integration][toolchanger][tool_offset_cal]") {
    helix::PrinterState& ps = get_printer_state();
    helix::PrinterStateTestAccess::pin_z_offset_strategy(
        ps, helix::ZOffsetCalibrationStrategy::PROBE_CALIBRATE);
    REQUIRE(lv_subject_get_int(ps.get_gcode_z_offset_subject()) == 0);

    helix::ui::ToolOffsetCalibrationPanel panel;
    panel.init_subjects();
    panel.on_activate();
    client->clear_gcode_script_history();
    panel.send_save();
    pump_until([] { return false; }, 20);

    for (const auto& script : client->gcode_script_history()) {
        CHECK(script.find("Z_OFFSET_APPLY") == std::string::npos);
    }

    panel.on_deactivate();
    panel.cleanup();
}
