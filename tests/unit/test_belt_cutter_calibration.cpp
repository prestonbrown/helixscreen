// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_belt_cutter_calibration.cpp
 * @brief Motorized belt tensioning and cutter calibration: which printers offer
 *        them, the gcode each sends, and the print-in-progress refusal.
 */

#include "ui_belt_cutter_cal.h"
#include "ui_toast_manager.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/printer_capture.h"
#include "../test_helpers/toast_log.h"
#include "app_globals.h"
#include "belt_cutter_calibration.h"
#include "capability_overrides.h"
#include "core/update_queue.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_capabilities_state.h"
#include "printer_discovery.h"
#include "printer_state.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrinterDiscovery;
using nlohmann::json;
namespace cal = helix::belt_cutter_cal;

namespace {

/// The Kalico port's K2 Plus, from its real config.
PrinterDiscovery kalico_k2() {
    const json capture = helix::test::load_printer_capture("creality_k2_plus_jacobean");
    PrinterDiscovery hw;
    hw.parse_objects(capture.at("printer_objects"));
    hw.set_is_kalico(true);
    return hw;
}

PrinterDiscovery printer_with(json objects, bool kalico) {
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    hw.set_is_kalico(kalico);
    return hw;
}

} // namespace

TEST_CASE("belt/cutter cal: the Kalico K2 port offers both, with its own commands",
          "[belt_cutter_cal]") {
    const PrinterDiscovery hw = kalico_k2();
    REQUIRE(cal::can_tension_belts(hw));
    REQUIRE(cal::can_calibrate_cutter(hw));
    CHECK(cal::belt_tension_gcode(hw, cal::BeltAxes::Both) == "BELT_TENSION");
    CHECK(cal::belt_tension_gcode(hw, cal::BeltAxes::X) == "BELT_TENSION AXES=X");
    CHECK(cal::belt_tension_gcode(hw, cal::BeltAxes::Y) == "BELT_TENSION AXES=Y");
    // A Python command, not a gcode_macro: nothing in the objects list names it.
    CHECK(cal::cutter_calibration_gcode(hw) == "CALIBRATE_CUT_POS");
}

TEST_CASE("belt/cutter cal: hidden on every printer the table has no row for",
          "[belt_cutter_cal]") {
    // Stock K2 firmware publishes the same tensioners and macros; its commands
    // are not confirmed on hardware, so it has no row yet.
    const json stock_k2 = {"box",
                           "motor_control",
                           "belt_mdl mdlx",
                           "belt_mdl mdly",
                           "gcode_macro BELT_TENSION",
                           "gcode_macro CALIBRATE_CUT_POS"};
    const PrinterDiscovery hw_stock = printer_with(stock_k2, /*kalico=*/false);
    CHECK_FALSE(cal::can_tension_belts(hw_stock));
    CHECK_FALSE(cal::can_calibrate_cutter(hw_stock));
    CHECK(cal::belt_tension_gcode(hw_stock, cal::BeltAxes::Both).empty());
    CHECK(cal::cutter_calibration_gcode(hw_stock).empty());

    // Kalico on any other printer.
    const PrinterDiscovery voron =
        printer_with({"box", "gcode_macro BELT_TENSION", "stepper_x"}, /*kalico=*/true);
    CHECK_FALSE(cal::can_tension_belts(voron));
    CHECK_FALSE(cal::can_calibrate_cutter(voron));
}

TEST_CASE("belt/cutter cal: each routine needs its own hardware on the Kalico port",
          "[belt_cutter_cal]") {
    // Tensioners without the macro that drives them; no box for the cutter.
    const PrinterDiscovery no_macro =
        printer_with({"belt_mdl x", "belt_mdl y", "motor_control"}, /*kalico=*/true);
    CHECK_FALSE(cal::can_tension_belts(no_macro));
    CHECK_FALSE(cal::can_calibrate_cutter(no_macro));

    // The cutter alone: motor_control and box, no tensioners.
    const PrinterDiscovery cutter_only = printer_with({"motor_control", "box"}, /*kalico=*/true);
    CHECK_FALSE(cal::can_tension_belts(cutter_only));
    CHECK(cal::cutter_calibration_gcode(cutter_only) == "CALIBRATE_CUT_POS");
}

TEST_CASE("belt/cutter cal: discovery sets the Calibration rows' subjects", "[belt_cutter_cal]") {
    LVGLTestFixture fixture;
    helix::PrinterCapabilitiesState caps;
    caps.init_subjects(false);
    auto value = [&caps](helix::Capability c) { return lv_subject_get_int(caps.subject(c)); };

    const PrinterDiscovery hw = kalico_k2();
    CapabilityOverrides overrides;
    overrides.set_hardware(hw);
    caps.set_hardware(hw, overrides);
    CHECK(value(helix::Capability::CanTensionBelts) == 1);
    CHECK(value(helix::Capability::CanCalibrateCutter) == 1);

    PrinterDiscovery bare;
    bare.parse_objects(json{"fan"});
    CapabilityOverrides bare_overrides;
    bare_overrides.set_hardware(bare);
    caps.set_hardware(bare, bare_overrides);
    CHECK(value(helix::Capability::CanTensionBelts) == 0);
    CHECK(value(helix::Capability::CanCalibrateCutter) == 0);
}

namespace {

class BeltCutterSendFixture : public LVGLTestFixture {
  public:
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    MoonrakerAPIMock api{client, state};

    BeltCutterSendFixture() {
        state.init_subjects(false);
        get_printer_state().init_subjects(false);
        client.clear_gcode_script_history();
    }

    void settle() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    std::vector<std::string> sent() const {
        return client.gcode_script_history();
    }
};

} // namespace

TEST_CASE_METHOD(BeltCutterSendFixture, "belt/cutter cal: each button sends its routine",
                 "[belt_cutter_cal]") {
    const PrinterDiscovery hw = kalico_k2();

    SECTION("one belt") {
        helix::ui::start_belt_tension(&api, hw, cal::BeltAxes::Y);
        settle();
        CHECK(sent() == std::vector<std::string>{"BELT_TENSION AXES=Y"});
    }
    SECTION("the cutter") {
        helix::ui::start_cutter_calibration(&api, hw);
        settle();
        CHECK(sent() == std::vector<std::string>{"CALIBRATE_CUT_POS"});
    }
    SECTION("a printer without either sends nothing") {
        const PrinterDiscovery voron = printer_with({"stepper_x"}, /*kalico=*/false);
        helix::ui::start_belt_tension(&api, voron, cal::BeltAxes::Both);
        helix::ui::start_cutter_calibration(&api, voron);
        settle();
        CHECK(sent().empty());
    }
}

TEST_CASE_METHOD(BeltCutterSendFixture, "belt/cutter cal: refused while a print holds the machine",
                 "[belt_cutter_cal]") {
    helix::test::ToastLog log;
    const PrinterDiscovery hw = kalico_k2();
    lv_subject_t* blocked = get_printer_state().print_state().get_machine_motion_blocked_subject();
    lv_subject_set_int(blocked, 1);

    helix::ui::start_belt_tension(&api, hw, cal::BeltAxes::Both);
    helix::ui::start_cutter_calibration(&api, hw);
    settle();

    lv_subject_set_int(blocked, 0);
    CHECK(sent().empty());
    REQUIRE(log.shown.size() == 2);
    for (const auto& toast : log.shown) {
        CHECK(toast.severity == ToastSeverity::WARNING);
        CHECK(toast.message == "Cannot calibrate while printing");
    }
}
