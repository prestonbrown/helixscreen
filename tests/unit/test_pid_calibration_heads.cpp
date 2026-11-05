// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Which heater the heater calibration tunes on a printer with several heads
// (prestonbrown/helixscreen#1870).

#include "ui_panel_calibration_pid.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/pid_calibration_panel_test_access.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/printer_state_test_access.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "tool_state.h"

#include <algorithm>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using Access = helix::ui::PIDCalibrationPanelTestAccess;

namespace {

void load_tools(const nlohmann::json& objects) {
    helix::PrinterDiscovery hw;
    hw.parse_objects(objects);
    auto& ts = helix::ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);
    ts.init_tools(hw);
}

struct PidHeadsFixture : LVGLTestFixture {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    MoonrakerAPI api{client, state};
    PIDCalibrationPanel panel;

    PidHeadsFixture() {
        panel.init_subjects();
        panel.set_api(&api);
    }
    ~PidHeadsFixture() override {
        helix::ui::UpdateQueue::instance().drain();
        load_tools(nlohmann::json::array({"extruder", "heater_bed"}));
    }

    void toolchanger(int active_tool) {
        load_tools(nlohmann::json::array(
            {"toolchanger", "tool T0", "tool T1", "extruder", "extruder1", "heater_bed"}));
        helix::ToolState::instance().update_from_status(
            {{"toolchanger", {{"tool_number", active_tool}}}});
    }

    void set_active_tool(int tool) {
        helix::ToolState::instance().update_from_status({{"toolchanger", {{"tool_number", tool}}}});
        helix::ui::UpdateQueue::instance().drain();
    }

    static int subject_int(const char* name) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, name);
        REQUIRE(s != nullptr);
        return lv_subject_get_int(s);
    }

    void start() {
        client.clear_gcode_script_history();
        Access::start_clicked(panel);
        helix::ui::UpdateQueue::instance().drain();
    }

    bool sent(const std::string& needle) const {
        const auto& h = client.gcode_script_history();
        return std::any_of(h.begin(), h.end(), [&](const std::string& line) {
            return line.find(needle) != std::string::npos;
        });
    }
};

} // namespace

TEST_CASE_METHOD(PidHeadsFixture, "Heater calibration tunes the active tool's heater",
                 "[pid][pid_heads]") {
    toolchanger(1);
    Access::activate(panel);
    start();
    CHECK(sent("PID_CALIBRATE HEATER=extruder1 "));
    CHECK_FALSE(sent("HEATER=extruder "));
}

TEST_CASE_METHOD(PidHeadsFixture, "Heater calibration tunes the head the user picks",
                 "[pid][pid_heads]") {
    toolchanger(1);
    Access::activate(panel);
    Access::head_clicked(panel, 0);
    start();
    CHECK(sent("PID_CALIBRATE HEATER=extruder "));
}

TEST_CASE_METHOD(PidHeadsFixture, "A single-extruder printer calibrates its extruder",
                 "[pid][pid_heads]") {
    load_tools(nlohmann::json::array({"extruder", "heater_bed"}));
    Access::activate(panel);
    start();
    CHECK(sent("PID_CALIBRATE HEATER=extruder "));
}

TEST_CASE_METHOD(PidHeadsFixture, "Picking the bed on a tool changer tunes the bed",
                 "[pid][pid_heads]") {
    toolchanger(1);
    Access::activate(panel);
    Access::bed_clicked(panel);
    start();
    CHECK(sent("PID_CALIBRATE HEATER=heater_bed "));
}

TEST_CASE_METHOD(PidHeadsFixture, "The part fan runs only for the head holding the active tool",
                 "[pid][pid_heads]") {
    toolchanger(1);
    Access::activate(panel);

    SECTION("active head: the fan is set") {
        Access::set_fan_speed(panel, 50);
        start();
        CHECK(sent("M106"));
    }
    SECTION("parked head: M106 would cool the active tool, so none is sent") {
        Access::head_clicked(panel, 0);
        Access::set_fan_speed(panel, 50);
        start();
        CHECK(sent("PID_CALIBRATE HEATER=extruder "));
        CHECK_FALSE(sent("M106"));
    }
}

TEST_CASE_METHOD(PidHeadsFixture, "The tool chips mark the active tool's head",
                 "[pid][pid_heads]") {
    toolchanger(1);
    Access::activate(panel);
    CHECK(subject_int("pid_cal_multi_tool") == 1);
    CHECK(subject_int("pid_cal_tool_count") == 2);
    CHECK(subject_int("pid_cal_tool_selected_0") == 0);
    CHECK(subject_int("pid_cal_tool_selected_1") == 1);
    CHECK(lv_xml_get_event_cb(nullptr, "on_pid_cal_tool") != nullptr);
}

TEST_CASE_METHOD(PidHeadsFixture, "A run's part fan is turned off whatever tool is active by then",
                 "[pid][pid_heads]") {
    toolchanger(1);
    Access::activate(panel);
    Access::set_fan_speed(panel, 50);
    start();
    REQUIRE(sent("M106"));

    set_active_tool(0);
    client.clear_gcode_script_history();
    Access::turn_off_fan(panel);
    CHECK(sent("M107"));
}

TEST_CASE_METHOD(PidHeadsFixture, "No M107 for a run that never set the fan", "[pid][pid_heads]") {
    toolchanger(1);
    Access::activate(panel);
    Access::head_clicked(panel, 0);
    Access::set_fan_speed(panel, 50);
    start();
    client.clear_gcode_script_history();
    Access::turn_off_fan(panel);
    CHECK_FALSE(sent("M107"));
}

TEST_CASE_METHOD(PidHeadsFixture, "MPC calibration tunes the picked tool's heater",
                 "[pid][pid_heads]") {
    toolchanger(1);
    Access::activate(panel);
    Access::use_mpc(panel);
    start();
    CHECK(sent("MPC_CALIBRATE HEATER=extruder1 "));
}

TEST_CASE_METHOD(PidHeadsFixture, "A closed panel stops following the active tool",
                 "[pid][pid_heads]") {
    toolchanger(1);
    Access::activate(panel);
    REQUIRE(subject_int("cal_show_pid_fan") == 1);

    panel.cleanup();
    set_active_tool(0);
    CHECK(subject_int("cal_show_pid_fan") == 1);
}

TEST_CASE_METHOD(PidHeadsFixture, "An open panel hides the fan when its tool is parked",
                 "[pid][pid_heads]") {
    toolchanger(1);
    Access::activate(panel);
    set_active_tool(0);
    CHECK(subject_int("cal_show_pid_fan") == 0);
}

// Klipper runs one command at a time, so a print running means no calibration
// is: TURN_OFF_HEATERS on the way out would stop that print's heaters.
TEST_CASE_METHOD(
    PidHeadsFixture,
    "Leaving a calibrating panel turns heaters and fan off only when no print is running",
    "[pid][pid_heads]") {
    helix::PrinterStateTestAccess::reset(get_printer_state());
    get_printer_state().init_subjects(false);
    load_tools(nlohmann::json::array({"extruder", "heater_bed"}));
    Access::activate(panel);
    Access::set_fan_speed(panel, 50);
    start();
    REQUIRE(sent("PID_CALIBRATE HEATER=extruder "));
    REQUIRE(sent("M106"));
    client.clear_gcode_script_history();

    SECTION("idle printer: the abandoned calibration's heaters go off") {
        Access::navigate_away(panel);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(sent("TURN_OFF_HEATERS"));
        CHECK(sent("M107"));
    }
    SECTION("a print is running: its heaters are left alone") {
        helix::test::set_wire_state(get_printer_state(), helix::PrintJobState::PRINTING);
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(get_printer_state().print_state().get_print_lifecycle() == PrintState::Printing);
        Access::navigate_away(panel);
        helix::ui::UpdateQueue::instance().drain();
        CHECK_FALSE(sent("TURN_OFF_HEATERS"));
        CHECK_FALSE(sent("M107"));
        helix::test::set_wire_state(get_printer_state(), helix::PrintJobState::STANDBY);
        helix::ui::UpdateQueue::instance().drain();
    }
}
