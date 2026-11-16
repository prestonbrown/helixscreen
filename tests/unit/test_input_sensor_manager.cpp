// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_input_sensor_manager.cpp
 * @brief InputSensorManager: discovery, default roles, state parsing, saved
 *        settings, the door-check default and the Settings rows.
 *
 * Run with: ./build/bin/helix-tests "[input_sensors]"
 */

#include "ui_print_start_controller.h"
#include "ui_toast_manager.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/golden_json.h"
#include "../test_helpers/print_start_controller_test_access.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/sensor_manager_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "config.h"
#include "core/update_queue.h"
#include "input_sensor_manager.h"
#include "input_sensor_types.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "tool_state.h"

#include <fstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::sensors;
using json = nlohmann::json;

namespace helix::sensors {
class InputSensorManagerTestAccess {
  public:
    static void reset(InputSensorManager& obj) {
        obj.set_sync_mode(true);
        SensorManagerTestAccess::reconcile(obj, {});
        std::lock_guard<std::recursive_mutex> lock(obj.mutex());
        obj.readings_.clear();
        obj.door_check_.reset();
        obj.known_.clear();
        obj.config_reactions_.clear();
        obj.open_doors_seen_.clear();
        obj.status_frame_ = false;
    }

    static bool inverted(InputSensorManager& obj, const std::string& klipper_name) {
        std::lock_guard<std::recursive_mutex> lock(obj.mutex());
        return obj.reading_locked(klipper_name).inverted;
    }
};
} // namespace helix::sensors

namespace {

const std::vector<std::string> kC5Objects = {
    "extruder",
    "gcode_button frontDoor",
    "gcode_button topDoor",
    "gcode_button extruder_pos1",
    "gcode_button extruder_grab1",
    "filament_switch_sensor runout",
};

constexpr uint8_t kOther = static_cast<uint8_t>(InputSensorRole::NONE);
constexpr uint8_t kDoor = static_cast<uint8_t>(InputSensorRole::DOOR);

/// Objects as printer.objects.list reports them, config sections as configfile.config does.
PrinterDiscovery discovery_of(const std::vector<std::string>& objects,
                              const json& config = json::object()) {
    PrinterDiscovery hw;
    hw.parse_objects(json(objects));
    hw.parse_config_keys(config);
    return hw;
}

class InputSensorFixture : public LVGLTestFixture {
  public:
    InputSensorFixture() {
        mgr().init_subjects();
        InputSensorManagerTestAccess::reset(mgr());
        config_slot() = json::object();
    }
    ~InputSensorFixture() override {
        InputSensorManagerTestAccess::reset(mgr());
        config_slot() = json::object();
    }

  protected:
    static InputSensorManager& mgr() {
        return InputSensorManager::instance();
    }
    static json& config_slot() {
        return Config::get_instance()->get_json(Config::get_instance()->df() + "input_sensors");
    }
    static void discover(const std::vector<std::string>& objects,
                         const json& config = json::object()) {
        mgr().discover(discovery_of(objects, config));
    }
    static void set_role(const std::string& name, uint8_t role) {
        mgr().set_role(name, role);
    }
    static uint8_t role_of_name(const std::string& name) {
        SensorEntry e;
        REQUIRE(mgr().find(name, e));
        return e.role;
    }
    static void report(const std::string& name, const char* state) {
        mgr().update_from_status(json{{name, {{"state", state}}}});
    }
    static std::string subject_string(const char* name) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, name);
        REQUIRE(s != nullptr);
        return lv_subject_get_string(s);
    }
    static int subject_int(const char* name) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, name);
        REQUIRE(s != nullptr);
        return lv_subject_get_int(s);
    }
};

} // namespace

TEST_CASE("default_input_role: door, lid and cover names are doors", "[input_sensors]") {
    CHECK(default_input_role("frontDoor") == InputSensorRole::DOOR);
    CHECK(default_input_role("TOP_LID") == InputSensorRole::DOOR);
    CHECK(default_input_role("cover_switch") == InputSensorRole::DOOR);
    CHECK(default_input_role("extruder_grab1") == InputSensorRole::NONE);
    CHECK(default_input_role("motor_a") == InputSensorRole::NONE);
}

TEST_CASE("input_reads_open: a door reads open when released, unless inverted", "[input_sensors]") {
    const auto door = InputSensorRole::DOOR;
    InputReading reading;
    reading.reported = true;

    reading.pressed = true;
    CHECK_FALSE(input_reads_open(door, reading));
    reading.pressed = false;
    CHECK(input_reads_open(door, reading));

    reading.inverted = true;
    CHECK_FALSE(input_reads_open(door, reading));
    reading.pressed = true;
    CHECK(input_reads_open(door, reading));

    // No state yet, or not a door: never open.
    reading.reported = false;
    CHECK_FALSE(input_reads_open(door, reading));
    reading.reported = true;
    CHECK_FALSE(input_reads_open(InputSensorRole::NONE, reading));
}

TEST_CASE_METHOD(InputSensorFixture,
                 "InputSensorManager discovers every gcode_button with default roles",
                 "[input_sensors]") {
    discover(kC5Objects);
    const auto inputs = mgr().entries();
    REQUIRE(inputs.size() == 4);
    CHECK(inputs[0].display_name == "frontDoor");
    CHECK(inputs[0].role == kDoor);
    CHECK(inputs[1].role == kDoor);
    CHECK(inputs[2].display_name == "extruder_pos1");
    CHECK(inputs[2].role == kOther);
    CHECK(inputs[3].role == kOther);
    SensorEntry other;
    CHECK_FALSE(mgr().find("filament_switch_sensor runout", other));
}

TEST_CASE_METHOD(InputSensorFixture, "InputSensorManager parses PRESSED/RELEASED into open doors",
                 "[input_sensors]") {
    discover(kC5Objects);
    // Nothing reported yet: no door reads open.
    CHECK(mgr().open_doors().empty());

    report("gcode_button frontDoor", "PRESSED");
    report("gcode_button topDoor", "RELEASED");
    CHECK(mgr().open_doors() == std::vector<std::string>{"topDoor"});

    // An unknown state word or a non-string is ignored, keeping the last state.
    report("gcode_button topDoor", "WEIRD");
    mgr().update_from_status(json{{"gcode_button topDoor", {{"state", nullptr}}}});
    CHECK(mgr().open_doors() == std::vector<std::string>{"topDoor"});

    report("gcode_button topDoor", "PRESSED");
    CHECK(mgr().open_doors().empty());
    // An unknown word does not read as released either.
    report("gcode_button topDoor", "WEIRD");
    CHECK(mgr().open_doors().empty());

    // A released non-door input is never an open door.
    report("gcode_button extruder_grab1", "RELEASED");
    CHECK(mgr().open_doors().empty());
}

TEST_CASE_METHOD(InputSensorFixture, "InputSensorManager door check defaults to having a door",
                 "[input_sensors]") {
    discover({"gcode_button extruder_grab1"});
    CHECK_FALSE(mgr().has_door_input());
    CHECK_FALSE(mgr().door_check_enabled());

    discover(kC5Objects);
    CHECK(mgr().has_door_input());
    CHECK(mgr().door_check_enabled());
    CHECK(subject_int("input_door_check") == 1);

    mgr().set_door_check_enabled(false);
    CHECK_FALSE(mgr().door_check_enabled());
    CHECK(subject_int("input_door_check") == 0);
}

TEST_CASE_METHOD(InputSensorFixture, "InputSensorManager persists roles, invert and door check",
                 "[input_sensors]") {
    discover(kC5Objects);
    set_role("gcode_button topDoor", kOther);
    set_role("gcode_button extruder_pos1", kDoor);
    mgr().set_option("gcode_button frontDoor", true);
    mgr().set_door_check_enabled(false);
    mgr().save_settings();
    const json saved = config_slot();

    // A fresh discovery restores the name defaults; the saved file overrides them.
    InputSensorManagerTestAccess::reset(mgr());
    config_slot() = saved;
    discover(kC5Objects);
    CHECK(role_of_name("gcode_button topDoor") == kDoor);
    CHECK_FALSE(InputSensorManagerTestAccess::inverted(mgr(), "gcode_button frontDoor"));
    CHECK(mgr().door_check_enabled());

    mgr().load_settings();
    CHECK(InputSensorManagerTestAccess::inverted(mgr(), "gcode_button frontDoor"));
    CHECK(role_of_name("gcode_button topDoor") == kOther);
    CHECK(role_of_name("gcode_button extruder_pos1") == kDoor);
    CHECK_FALSE(mgr().door_check_enabled());

    // No saved door_check: back to the derived default.
    config_slot().erase("door_check");
    mgr().load_settings();
    CHECK(mgr().door_check_enabled());
}

TEST_CASE_METHOD(InputSensorFixture, "InputSensorManager saved config reads back byte-identically",
                 "[input_sensors]") {
    std::ifstream in(helix::test::golden_path("sensor_settings", "input"));
    REQUIRE(in.good());
    const json fixture = json::parse(in, nullptr, false);
    REQUIRE_FALSE(fixture.is_discarded());

    discover(kC5Objects);
    config_slot() = fixture;
    mgr().load_settings();

    // The saved "other" beats the door name rule, the saved door beats none.
    CHECK(role_of_name("gcode_button frontDoor") == kDoor);
    CHECK(role_of_name("gcode_button topDoor") == kOther);
    CHECK(role_of_name("gcode_button extruder_pos1") == kDoor);
    CHECK(role_of_name("gcode_button extruder_grab1") == kOther);
    CHECK(InputSensorManagerTestAccess::inverted(mgr(), "gcode_button frontDoor"));
    CHECK_FALSE(InputSensorManagerTestAccess::inverted(mgr(), "gcode_button topDoor"));
    CHECK_FALSE(mgr().door_check_enabled());

    config_slot() = json::object();
    mgr().save_settings();
    CHECK(config_slot().dump() == fixture.dump());
}

TEST_CASE_METHOD(InputSensorFixture, "InputSensorManager rediscovery keeps the user's role choice",
                 "[input_sensors]") {
    discover(kC5Objects);
    set_role("gcode_button topDoor", kOther);
    set_role("gcode_button extruder_pos1", kDoor);

    // The configfile pass runs discovery again without reloading settings.
    discover(kC5Objects, json{{"gcode_button topDoor", {{"pin", "PB0"}}}});
    CHECK(role_of_name("gcode_button topDoor") == kOther);
    CHECK(role_of_name("gcode_button extruder_pos1") == kDoor);

    // An input new to a later discovery gets the name default.
    discover({"gcode_button topDoor", "gcode_button lid_switch"});
    CHECK(role_of_name("gcode_button topDoor") == kOther);
    CHECK(role_of_name("gcode_button lid_switch") == kDoor);
}

TEST_CASE_METHOD(InputSensorFixture, "InputSensorManager rows carry state, role and Invert",
                 "[input_sensors]") {
    discover(kC5Objects);
    REQUIRE(mgr().sensor_count() == 4);
    CHECK(mgr().row(0).name == "frontDoor");
    CHECK(mgr().row(0).role_count == 2);
    CHECK(mgr().row(0).selected_role == 1);
    CHECK(mgr().row(2).selected_role == 0);
    CHECK(mgr().row(0).state == "--");
    CHECK(mgr().row(0).toggle == RowToggle::Invert);
    CHECK_FALSE(mgr().row(0).toggle_value);

    report("gcode_button frontDoor", "RELEASED");
    report("gcode_button extruder_grab1", "PRESSED");
    CHECK(mgr().row(0).state == "Opened");
    CHECK(mgr().row(0).alert);
    CHECK(mgr().row(3).state == "Pressed");
    CHECK_FALSE(mgr().row(3).alert);

    mgr().set_option("gcode_button frontDoor", true);
    CHECK(mgr().row(0).state == "Closed");
    CHECK_FALSE(mgr().row(0).alert);
    CHECK(mgr().row(0).toggle_value);
}

TEST_CASE("doors_newly_open: only doors absent before are owed a warning", "[input_sensors]") {
    CHECK(doors_newly_open({}, {"frontDoor"}) == std::vector<std::string>{"frontDoor"});
    CHECK(doors_newly_open({"frontDoor"}, {"frontDoor"}).empty());
    CHECK(doors_newly_open({"frontDoor"}, {"frontDoor", "topDoor"}) ==
          std::vector<std::string>{"topDoor"});
    CHECK(doors_newly_open({"frontDoor", "topDoor"}, {}).empty());
}

TEST_CASE("input_config_reaction: an edge counts only with non-blank gcode", "[input_sensors]") {
    const auto r = input_config_reaction(
        json{{"press_gcode", "  \n\t "}, {"release_gcode", "\nRESPOND MSG=open"}});
    CHECK_FALSE(r.on_press);
    CHECK(r.on_release);

    const auto none = input_config_reaction(json{{"pin", "PB0"}});
    CHECK_FALSE(none.on_press);
    CHECK_FALSE(none.on_release);
    // A non-string value is not gcode.
    CHECK_FALSE(input_config_reaction(json{{"release_gcode", 1}}).on_release);
}

TEST_CASE("firmware_reacts_to_opening: a door opens on release, or on press when inverted",
          "[input_sensors]") {
    const InputConfigReaction on_release{false, true};
    const InputConfigReaction on_press{true, false};
    CHECK(firmware_reacts_to_opening(on_release, false));
    CHECK_FALSE(firmware_reacts_to_opening(on_release, true));
    CHECK(firmware_reacts_to_opening(on_press, true));
    CHECK_FALSE(firmware_reacts_to_opening(on_press, false));
}

TEST_CASE_METHOD(InputSensorFixture, "InputSensorManager reads which doors the printer announces",
                 "[input_sensors]") {
    // Before the configfile arrives the printer says nothing.
    discover(kC5Objects);
    CHECK_FALSE(mgr().firmware_reacts_to_opening("gcode_button frontDoor"));

    discover(kC5Objects,
             json{
                 {"gcode_button frontDoor", {{"release_gcode", "\nRESPOND MSG=open"}}},
                 {"gcode_button topDoor", {{"release_gcode", ""}, {"press_gcode", "M117 closed"}}},
                 {"extruder", {{"release_gcode", "ignored: not a button"}}},
             });
    CHECK(mgr().firmware_reacts_to_opening("gcode_button frontDoor"));
    CHECK_FALSE(mgr().firmware_reacts_to_opening("gcode_button topDoor"));
    // No config section at all: the printer says nothing.
    CHECK_FALSE(mgr().firmware_reacts_to_opening("gcode_button extruder_grab1"));

    // Inverted, the top door opens on press, which its config does act on.
    mgr().set_option("gcode_button topDoor", true);
    CHECK(mgr().firmware_reacts_to_opening("gcode_button topDoor"));
    mgr().set_option("gcode_button frontDoor", true);
    CHECK_FALSE(mgr().firmware_reacts_to_opening("gcode_button frontDoor"));

    // A later discovery replaces the earlier read.
    discover(kC5Objects);
    CHECK_FALSE(mgr().firmware_reacts_to_opening("gcode_button topDoor"));
}

TEST_CASE_METHOD(InputSensorFixture,
                 "InputSensorManager warns mid-print only for enclosure materials",
                 "[input_sensors]") {
    discover(kC5Objects);
    const std::vector<std::string> opened = {"gcode_button frontDoor"};

    // A material that needs a warm chamber: warned, by short name.
    CHECK(mgr().mid_print_door_warnings(opened, {"PLA", "ASA"}) ==
          std::vector<std::string>{"frontDoor"});
    // PLA alone wants air: quiet. Unknown materials (empty) stay quiet too.
    CHECK(mgr().mid_print_door_warnings(opened, {"PLA"}).empty());
    CHECK(mgr().mid_print_door_warnings(opened, {}).empty());
    // Nothing newly open, or the check off: quiet.
    CHECK(mgr().mid_print_door_warnings({}, {"ABS"}).empty());
    mgr().set_door_check_enabled(false);
    CHECK(mgr().mid_print_door_warnings(opened, {"ABS"}).empty());
    mgr().set_door_check_enabled(true);

    // The printer announcing the door itself silences only that door.
    discover(kC5Objects, json{{"gcode_button frontDoor", {{"release_gcode", "RESPOND MSG=open"}}}});
    CHECK(mgr().mid_print_door_warnings({"gcode_button frontDoor", "gcode_button topDoor"},
                                        {"ABS"}) == std::vector<std::string>{"topDoor"});
}

namespace {
/// PrinterState's print lifecycle needs the app's subjects, which only the UI
/// fixture initialises.
class InputSensorPrintFixture : public LVGLUITestFixture {
  public:
    InputSensorPrintFixture() {
        InputSensorManager::instance().init_subjects();
        InputSensorManagerTestAccess::reset(InputSensorManager::instance());
    }
    ~InputSensorPrintFixture() override {
        InputSensorManagerTestAccess::reset(InputSensorManager::instance());
    }
};
} // namespace

TEST_CASE_METHOD(
    InputSensorPrintFixture,
    "InputSensorManager toasts a door opened mid-print and not a role or invert change",
    "[input_sensors][door]") {
    auto& ps = get_printer_state();
    helix::test::set_wire_state(ps, PrintJobState::PRINTING);
    helix::ui::UpdateQueue::instance().drain();
    ps.print_state().set_print_materials({"ABS"});
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(ps.print_state().print_materials() == std::vector<std::string>{"ABS"});
    struct Restore {
        ~Restore() {
            auto& st = get_printer_state();
            st.print_state().set_print_materials({});
            helix::test::set_wire_state(st, PrintJobState::STANDBY);
            helix::ui::UpdateQueue::instance().drain();
            helix::ui::set_test_toast_hook(nullptr);
        }
    } restore;
    REQUIRE(ps.print_state().get_print_lifecycle() == PrintState::Printing);

    std::vector<std::string> toasts;
    helix::ui::set_test_toast_hook([&toasts](ToastSeverity, const std::string& message, uint32_t) {
        toasts.push_back(message);
    });

    InputSensorManager::instance().discover(discovery_of(kC5Objects));
    InputSensorManager::instance().update_from_status(
        json{{"gcode_button frontDoor", {{"state", "PRESSED"}}},
             {"gcode_button extruder_pos1", {{"state", "RELEASED"}}}});
    REQUIRE(toasts.empty());

    // A released input set to Door now reads open, but nothing moved.
    InputSensorManager::instance().set_role("gcode_button extruder_pos1", kDoor);
    // Inverting a closed door makes it read open, and nothing moved either.
    InputSensorManager::instance().set_option("gcode_button frontDoor", true);
    CHECK(toasts.empty());

    // The front door really opening is what warns.
    InputSensorManager::instance().set_option("gcode_button frontDoor", false);
    InputSensorManager::instance().update_from_status(
        json{{"gcode_button frontDoor", {{"state", "RELEASED"}}}});
    REQUIRE(toasts.size() == 1);
    CHECK(toasts[0].find("frontDoor") != std::string::npos);
}

TEST_CASE_METHOD(InputSensorPrintFixture,
                 "PrintStartController hands the start checks the open doors and the toolheads",
                 "[input_sensors][door][print_start_controller]") {
    auto& inputs = InputSensorManager::instance();
    inputs.discover(discovery_of(kC5Objects));
    inputs.update_from_status(json{{"gcode_button frontDoor", {{"state", "RELEASED"}}},
                                   {"gcode_button topDoor", {{"state", "PRESSED"}}}});
    REQUIRE(inputs.door_check_enabled());

    ToolState::instance().deinit_subjects();
    ToolState::instance().init_subjects(true);
    helix::PrinterDiscovery changer;
    changer.parse_objects(
        json::array({"gcode_move", "toolchanger", "tool T0", "tool T1", "extruder", "extruder1"}));
    ToolState::instance().init_tools(changer);
    REQUIRE(ToolState::instance().has_multiple_toolheads());

    helix::ui::PrintStartController controller{get_printer_state(), nullptr};
    auto ctx = PrintStartControllerTestAccess::gather(controller);
    CHECK(ctx.door_check_enabled);
    CHECK(ctx.open_doors == std::vector<std::string>{"frontDoor"});
    CHECK(ctx.multiple_toolheads);

    inputs.set_door_check_enabled(false);
    helix::PrinterDiscovery single;
    single.parse_objects(json::array({"gcode_move", "extruder"}));
    ToolState::instance().init_tools(single);
    ctx = PrintStartControllerTestAccess::gather(controller);
    CHECK_FALSE(ctx.door_check_enabled);
    CHECK_FALSE(ctx.multiple_toolheads);
}
