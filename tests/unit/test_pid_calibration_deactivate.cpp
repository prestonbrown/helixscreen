// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// What leaving the heater calibration panel sends to the printer.

#include "ui_panel_calibration_pid.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/pid_calibration_panel_test_access.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/printer_state_test_access.h"
#include "app_globals.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <algorithm>
#include <string>

#include "../catch_amalgamated.hpp"

using Access = helix::ui::PIDCalibrationPanelTestAccess;

namespace {

struct PidDeactivateFixture : LVGLTestFixture {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    MoonrakerAPI api{client, state};
    PIDCalibrationPanel panel;

    PidDeactivateFixture() {
        helix::PrinterStateTestAccess::reset(get_printer_state());
        get_printer_state().init_subjects(false);
        state.init_subjects(false);
        state.set_klippy_state_sync(helix::KlippyState::READY);
        panel.init_subjects();
        panel.set_api(&api);
    }
    ~PidDeactivateFixture() override {
        helix::test::set_wire_state(get_printer_state(), helix::PrintJobState::STANDBY);
        helix::ui::UpdateQueue::instance().drain();
    }

    bool sent(const std::string& needle) const {
        const auto& h = client.gcode_script_history();
        return std::any_of(h.begin(), h.end(), [&](const std::string& line) {
            return line.find(needle) != std::string::npos;
        });
    }

    void print_state(helix::PrintJobState s) {
        helix::test::set_wire_state(get_printer_state(), s);
        helix::ui::UpdateQueue::instance().drain();
    }

    void leave() {
        Access::deactivate(panel);
        helix::ui::UpdateQueue::instance().drain();
    }
};

} // namespace

// Klipper runs one command at a time, so a print running means no calibration
// is: TURN_OFF_HEATERS or M107 on the way out would stop that print's heaters
// or part fan.
TEST_CASE_METHOD(PidDeactivateFixture,
                 "Leaving a calibrating panel turns heaters and fan off only when no print runs",
                 "[pid][pid_deactivate]") {
    Access::set_fan_speed(panel, 50);
    Access::start_clicked(panel);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(sent("PID_CALIBRATE HEATER=extruder"));
    client.clear_gcode_script_history();

    SECTION("idle printer: the abandoned calibration's heaters and fan go off") {
        leave();
        CHECK(sent("TURN_OFF_HEATERS"));
        CHECK(sent("M107"));
    }
    SECTION("a print is running: its heaters and fan are left alone") {
        print_state(helix::PrintJobState::PRINTING);
        REQUIRE(get_printer_state().get_print_lifecycle() == PrintState::Printing);
        leave();
        CHECK_FALSE(sent("TURN_OFF_HEATERS"));
        CHECK_FALSE(sent("M107"));
    }
    SECTION("a paused print: the calibration's fan goes off, the heaters stay") {
        print_state(helix::PrintJobState::PAUSED);
        REQUIRE(get_printer_state().get_print_lifecycle() == PrintState::Paused);
        leave();
        CHECK(sent("M107"));
        CHECK_FALSE(sent("TURN_OFF_HEATERS"));
    }
}
