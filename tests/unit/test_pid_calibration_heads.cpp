// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Which heater the heater calibration tunes on a printer with several heads
// (prestonbrown/helixscreen#1870).

#include "ui_panel_calibration_pid.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/pid_calibration_panel_test_access.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "tool_state.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

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

TEST_CASE_METHOD(PidHeadsFixture, "Calibration tunes the heater the panel targets",
                 "[pid][pid_heads]") {
    const char* expected = nullptr;
    SECTION("a tool changer's active tool") {
        toolchanger(1);
        Access::activate(panel);
        expected = "PID_CALIBRATE HEATER=extruder1 ";
    }
    SECTION("the head the user picks") {
        toolchanger(1);
        Access::activate(panel);
        Access::head_clicked(panel, 0);
        expected = "PID_CALIBRATE HEATER=extruder ";
    }
    SECTION("a single-extruder printer's extruder") {
        load_tools(nlohmann::json::array({"extruder", "heater_bed"}));
        Access::activate(panel);
        expected = "PID_CALIBRATE HEATER=extruder ";
    }
    SECTION("the bed, picked on a tool changer") {
        toolchanger(1);
        Access::activate(panel);
        Access::bed_clicked(panel);
        expected = "PID_CALIBRATE HEATER=heater_bed ";
    }
    SECTION("MPC on the active tool") {
        toolchanger(1);
        Access::activate(panel);
        Access::use_mpc(panel);
        expected = "MPC_CALIBRATE HEATER=extruder1 ";
    }
    start();
    CHECK(sent(expected));
    // One heater per run: the other head is never named too.
    CHECK(sent("HEATER=extruder ") ==
          (std::string(expected).find("extruder ") != std::string::npos));
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

TEST_CASE_METHOD(PidHeadsFixture, "MPC migration names the fan that cools the picked head",
                 "[pid][pid_heads][mpc_migration]") {
    auto cooling_fan = [&]() -> std::string {
        for (const auto& e : Access::mpc_migration_edits(panel)) {
            if (e.key == "cooling_fan" && e.type == helix::system::ConfigEdit::Type::ADD_IF_ABSENT)
                return e.value;
        }
        return "";
    };

    SECTION("single extruder: Klipper's [fan]") {
        load_tools(nlohmann::json::array({"extruder", "heater_bed"}));
        api.hardware().parse_objects(nlohmann::json::array({"extruder", "heater_bed", "fan"}));
        Access::activate(panel);
        CHECK(cooling_fan() == "fan");
    }
    SECTION("single extruder with no [fan]: no cooling_fan to name") {
        load_tools(nlohmann::json::array({"extruder", "heater_bed"}));
        api.hardware().parse_objects(
            nlohmann::json::array({"extruder", "heater_bed", "fan_generic part"}));
        Access::activate(panel);
        CHECK(cooling_fan().empty());
    }
    SECTION("tool changer: the active head's own fan") {
        api.hardware().parse_objects(
            nlohmann::json::array({"toolchanger", "tool T0", "tool T1", "extruder", "extruder1",
                                   "heater_bed", "fan_generic T1_partfan"}));
        toolchanger(1);
        helix::ToolState::instance().update_from_status(
            {{"tool T1", {{"fan", "fan_generic T1_partfan"}}}});
        Access::activate(panel);
        CHECK(cooling_fan() == "fan_generic T1_partfan");

        Access::head_clicked(panel, 0); // parked: no part fan reaches it
        CHECK(cooling_fan().empty());
    }
    SECTION("the bed has no cooling fan") {
        load_tools(nlohmann::json::array({"extruder", "heater_bed"}));
        Access::activate(panel);
        Access::bed_clicked(panel);
        CHECK(cooling_fan().empty());
    }
}

TEST_CASE_METHOD(PidHeadsFixture, "An MPC heater with no cooling_fan is offered the part fan",
                 "[pid][pid_heads][mpc_migration]") {
    struct KalicoScope {
        KalicoScope() {
            setenv("HELIX_MOCK_KALICO", "1", 1);
        }
        ~KalicoScope() {
            get_printer_state().set_hardware(helix::PrinterDiscovery{});
            unsetenv("HELIX_MOCK_KALICO");
        }
    } kalico_scope;
    helix::PrinterDiscovery kalico;
    kalico.set_is_kalico(true);
    get_printer_state().set_hardware(kalico);
    load_tools(nlohmann::json::array({"extruder", "heater_bed"}));
    api.hardware().parse_objects(nlohmann::json::array({"extruder", "heater_bed", "fan"}));

    auto activate_and_detect = [&]() {
        Access::activate(panel);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        helix::ui::UpdateQueue::instance().drain();
    };

    SECTION("taking the offer stops at the confirmation, before any calibration") {
        activate_and_detect();
        REQUIRE(subject_int("cal_offer_cooling_fan") == 1);
        lv_subject_set_int(lv_xml_get_subject(nullptr, "cal_add_cooling_fan"), 1);
        start();
        CHECK_FALSE(sent("MPC_CALIBRATE"));
    }
    SECTION("declining calibrates with no config change") {
        activate_and_detect();
        REQUIRE(subject_int("cal_offer_cooling_fan") == 1);
        start();
        CHECK(sent("MPC_CALIBRATE HEATER=extruder "));
    }
    SECTION("a heater that names a fan gets no offer") {
        client.set_extruder_cooling_fan("fan_generic mine");
        activate_and_detect();
        CHECK(subject_int("cal_offer_cooling_fan") == 0);
    }
    SECTION("leaving the offer behind drops the choice") {
        activate_and_detect();
        lv_subject_set_int(lv_xml_get_subject(nullptr, "cal_add_cooling_fan"), 1);
        Access::bed_clicked(panel);
        CHECK(subject_int("cal_offer_cooling_fan") == 0);
        CHECK(subject_int("cal_add_cooling_fan") == 0);
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

TEST_CASE_METHOD(PidHeadsFixture,
                 "A run's part fan is turned off whatever tool is active by then, and only if set",
                 "[pid][pid_heads]") {
    toolchanger(1);
    Access::activate(panel);
    bool fan_set = true;
    SECTION("the run set the active head's fan") {
        Access::set_fan_speed(panel, 50);
        start();
        REQUIRE(sent("M106"));
        set_active_tool(0);
    }
    SECTION("a parked head's run never set it") {
        Access::head_clicked(panel, 0);
        Access::set_fan_speed(panel, 50);
        start();
        fan_set = false;
    }
    client.clear_gcode_script_history();
    Access::turn_off_fan(panel);
    CHECK(sent("M107") == fan_set);
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

// Klipper runs one command at a time, so a running print owns the heaters and
// the fan. A pause accepts commands, so a calibration's own fan still goes off.
TEST_CASE_METHOD(PidHeadsFixture, "Leaving a calibrating panel stops only what no print owns",
                 "[pid][pid_heads]") {
    using helix::PrintJobState;
    const auto [job, heaters_off, fan_off] = GENERATE(table<PrintJobState, bool, bool>({
        {PrintJobState::STANDBY, true, true},
        {PrintJobState::PRINTING, false, false},
        {PrintJobState::PAUSED, false, true},
    }));
    get_printer_state().init_subjects(false);
    load_tools(nlohmann::json::array({"extruder", "heater_bed"}));
    Access::activate(panel);
    Access::set_fan_speed(panel, 50);
    start();
    REQUIRE(sent("M106"));

    helix::test::set_wire_state(get_printer_state(), job);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(job_holds_machine(get_printer_state().print_state().get_print_lifecycle()) ==
            (job != PrintJobState::STANDBY));
    client.clear_gcode_script_history();
    panel.on_deactivating(DeactivateReason::NavigateAway);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(sent("TURN_OFF_HEATERS") == heaters_off);
    CHECK(sent("M107") == fan_off);
    helix::test::set_wire_state(get_printer_state(), PrintJobState::STANDBY);
}

namespace {
std::string progress_text() {
    lv_subject_t* s = lv_xml_get_subject(nullptr, "pid_progress_text");
    REQUIRE(s != nullptr);
    return lv_subject_get_string(s);
}
void discover_extruder() {
    get_printer_state().init_subjects(false);
    helix::PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json::array({"extruder", "heater_bed"}));
    get_printer_state().set_hardware(hw);
    get_printer_state().temperature_state().init_extruders(hw.heaters());
    load_tools(nlohmann::json::array({"extruder", "heater_bed"}));
}
void nozzle_at(double c) {
    get_printer_state().update_from_status({{"extruder", {{"temperature", c}}}});
    helix::ui::UpdateQueue::instance().drain();
}
} // namespace

// The screensaver suspends the panel mid-run; the printer keeps calibrating,
// so on waking the progress has to follow the heater again.
TEST_CASE_METHOD(PidHeadsFixture, "A calibration's progress follows the heater after a wake",
                 "[pid][pid_progress]") {
    discover_extruder();
    panel.on_activate();
    start();
    nozzle_at(100.0);
    REQUIRE(progress_text().find("100") != std::string::npos);

    panel.on_deactivate(DeactivateReason::Suspended);
    nozzle_at(140.0);
    panel.on_activate();
    nozzle_at(160.0);
    CHECK(progress_text().find("160") != std::string::npos);
}

// MPC_CALIBRATE narrates its own phases; the PID heat-up wording must not
// replace them on every temperature report.
TEST_CASE_METHOD(PidHeadsFixture, "An MPC run shows the firmware's phase, not PID heat-up text",
                 "[pid][pid_progress]") {
    discover_extruder();
    panel.on_activate();
    Access::use_mpc(panel);
    start();
    REQUIRE(sent("MPC_CALIBRATE HEATER=extruder "));

    const auto phase_after = [&](const char* line) {
        client.dispatch_gcode_response(line);
        helix::ui::UpdateQueue::instance().drain();
        nozzle_at(150.0);
        return std::pair{progress_text(), subject_int("pid_cal_progress")};
    };
    // Kalico's control_mpc.py wording, as Klipper's respond_info marks it.
    const auto settle = phase_after("// Waiting for heater to drop below 45.0 degrees Celsius");
    const auto heatup = phase_after("// Performing heatup test, target is 220.0 degrees");
    const auto transfer =
        phase_after("// Performing ambient transfer tests, target is 236.0 degrees");
    CHECK(settle.first == "Waiting for heater to drop below 45.0 degrees Celsius");
    CHECK(heatup.first == "Performing heatup test, target is 220.0 degrees");
    CHECK(transfer.first == "Performing ambient transfer tests, target is 236.0 degrees");
    CHECK(settle.second > 0);
    CHECK(heatup.second > settle.second);
    CHECK(transfer.second > heatup.second);
}

// A run that finishes behind the screensaver still saves, and the wake shows
// its result rather than resetting the panel.
TEST_CASE_METHOD(PidHeadsFixture, "An MPC result that lands behind the screensaver is kept",
                 "[pid][pid_progress]") {
    discover_extruder();
    panel.on_activate();
    Access::use_mpc(panel);
    start();
    REQUIRE(subject_int("pid_cal_state") == 1); // CALIBRATING
    client.clear_gcode_script_history();

    panel.on_deactivate(DeactivateReason::Suspended);
    client.dispatch_gcode_response("// Finished MPC calibration of heater 'extruder'\n"
                                   "// Measured:\n"
                                   "// block_heat_capacity=17.9784 [J/K]\n"
                                   "// sensor_responsiveness=0.0971234 [K/s/K]\n"
                                   "// ambient_transfer=0.0892993 [W/K]\n"
                                   "// fan_ambient_transfer=0.0892993, 0.0940654 [W/K]");
    REQUIRE(wait_until([&] {
        helix::ui::UpdateQueue::instance().drain();
        return sent("SAVE_CONFIG");
    }));
    panel.on_activate();
    CHECK(subject_int("pid_cal_state") >= 2); // SAVING or COMPLETE, not reset to IDLE

    // Leaving for real after a later suspend: the next visit starts fresh.
    panel.on_deactivate(DeactivateReason::Suspended);
    panel.on_deactivate(DeactivateReason::NavigateAway);
    panel.on_activate();
    CHECK(subject_int("pid_cal_state") == 0);
}
