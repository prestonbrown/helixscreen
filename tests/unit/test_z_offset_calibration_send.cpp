// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_z_offset_calibration_send.cpp
 * @brief Wiring tests for the calibration panel's accept send under ZMOD.
 *
 * The FIRMWARE_MANAGED accept arm puts a bare SET_GCODE_OFFSET Z= on the wire.
 * ZMOD's override of that command persists `z - _TEST_POINT.temp_z_offset`
 * (ghzserg/zmod#699), and the variable survives END_PRINT/CANCEL_PRINT - so an
 * accept after a print stores the calibrated value minus that print's probe
 * delta. These tests pin that the stale-delta clear line rides in front of the
 * offset on ZMOD, and only there: never on a printer without the mechanism,
 * and never while a print is running (mid-print the subtraction excludes the
 * live per-print transient and is correct).
 *
 * Companion to test_print_tune_zoffset_adjust.cpp, which pins the same gate on
 * the tune overlay's baby-step path.
 */

#include "ui_panel_calibration_zoffset.h"

#include "../lvgl_test_fixture.h"
#include "app_globals.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "test_helpers/printer_state_test_access.h"
#include "test_helpers/update_queue_test_access.h"
#include "test_helpers/zoffset_calibration_test_access.h"

#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrinterState;
using helix::ZOffsetCalibrationStrategy;
using nlohmann::json;

namespace {

class CalibrationSendFixture : public LVGLTestFixture {
  public:
    CalibrationSendFixture() {
        helix::PrinterStateTestAccess::reset(state());
        state().init_subjects(false);
        helix::PrinterStateTestAccess::pin_z_offset_strategy(
            state(), ZOffsetCalibrationStrategy::FIRMWARE_MANAGED);
        state().set_klippy_state_sync(helix::KlippyState::READY);
        // The discovery snapshot persists on the process-wide state across test
        // cases; start each one plain so only set_zmod() cases see the provider.
        state().set_hardware(helix::PrinterDiscovery{});
    }

    ~CalibrationSendFixture() override {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    helix::PrinterState& state() {
        return get_printer_state();
    }

    /// ZMOD-shaped discovery snapshot, as Moonraker's macro listing produces.
    void set_zmod() {
        helix::PrinterDiscovery hw;
        hw.parse_objects(json::array({"gcode_macro SAVE_ZMOD_DATA"}));
        state().set_hardware(std::move(hw));
    }

    void set_printing(bool printing) {
        state().update_from_status(
            json{{"print_stats", json{{"state", printing ? "printing" : "standby"}}}});
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    /// Accept the calibrated -0.150 and return what went on the wire.
    std::string accept_sends() {
        MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
        MoonrakerAPI api(client, state());
        ZOffsetCalibrationPanel panel;
        panel.set_api(&api);
        ZOffsetCalibrationTestAccess::set_cumulative_delta(panel, -0.15f);
        ZOffsetCalibrationTestAccess::send_accept(panel);
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
        return client.last_send_script();
    }
};

} // namespace

TEST_CASE_METHOD(CalibrationSendFixture,
                 "Calibration accept: ZMOD clears the stale probe delta before the offset",
                 "[ui_integration][zoffset][zmod][regression]") {
    set_zmod();
    set_printing(false);

    CHECK(accept_sends() == "SET_GCODE_VARIABLE MACRO=_TEST_POINT VARIABLE=temp_z_offset VALUE=0\n"
                            "SET_GCODE_OFFSET Z=-0.150");
}

TEST_CASE_METHOD(CalibrationSendFixture,
                 "Calibration accept: no clear line without the firmware mechanism",
                 "[ui_integration][zoffset][regression]") {
    // Same FIRMWARE_MANAGED accept, but the printer is not ZMOD (Forge-X, or a
    // plain macro-persisted box): the offset goes out alone.
    set_printing(false);

    CHECK(accept_sends() == "SET_GCODE_OFFSET Z=-0.150");
}

TEST_CASE_METHOD(CalibrationSendFixture,
                 "Calibration accept: a running print keeps the probe delta intact",
                 "[ui_integration][zoffset][zmod][regression]") {
    // THE GATE. Mid-print temp_z_offset is the live transient the override is
    // supposed to subtract; zeroing it would corrupt the running job's stored
    // offset. Calibration is an idle activity, but the send must hold the same
    // gate the tune overlay does if it ever fires under one.
    set_zmod();
    set_printing(true);

    CHECK(accept_sends() == "SET_GCODE_OFFSET Z=-0.150");
}
