// SPDX-License-Identifier: GPL-3.0-or-later

// UI starts that heat the nozzle by name, and how they read a refusal from a
// shared-heater backend (INDX) with no tool on the head.

#include "ui_panel_calibration_pid.h"
#include "ui_panel_controls.h"
#include "ui_wizard_ams_identify.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_fixtures.h"
#include "../test_helpers/controls_panel_test_access.h"
#include "../test_helpers/pid_calibration_panel_test_access.h"
#include "../test_helpers/registered_backend.h"
#include "../test_helpers/toolchanger_test_helper.h"
#include "../test_helpers/wizard_ams_identify_test_access.h"
#include "ams_backend_indx.h"
#include "ams_state.h"
#include "core/update_queue.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "temperature_controller.h"
#include "temperature_service.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
using IndxHelper = helix::test::GcodeCaptureBackend<helix::AmsBackendIndx>;

namespace {

/// An INDX backend registered with AmsState, its head empty.
struct EmptyHeadIndx {
    helix::test::RegisteredBackend<IndxHelper> harness;
    EmptyHeadIndx() {
        helix::PrinterDiscovery hw;
        hw.parse_objects(json::array({"indx", "save_variables", "extruder",
                                      "gcode_macro TOOL_POSITIONS", "gcode_macro _TOOLCHANGE_VARS",
                                      "gcode_macro CHANGE_TOOL", "gcode_macro PARK_TOOL"}));
        harness->set_discovery(hw);
        harness->feed(json{{"save_variables", {{"variables", {{"active_tool", -1}}}}},
                           {"gcode_macro TOOL_POSITIONS", {{"tool_count", 4}}}});
        REQUIRE_FALSE(helix::AmsState::instance().nozzle_heat_blocked_reason().empty());
    }
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "AMS wizard names INDX by its display name",
                 "[ams][indx][wizard]") {
    EmptyHeadIndx indx;
    WizardAmsIdentifyStep step;
    CHECK(helix::ui::WizardAmsIdentifyStepTestAccess::type_name(step) == "INDX");
}

TEST_CASE_METHOD(LVGLTestFixture, "Extruder PID calibration does not start on a blocked nozzle",
                 "[ams][indx][heat][pid]") {
    EmptyHeadIndx indx;
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    MoonrakerAPI api(client, state);
    PIDCalibrationPanel panel;
    panel.init_subjects();
    panel.set_api(&api);
    client.clear_gcode_script_history();

    helix::ui::PIDCalibrationPanelTestAccess::start_clicked(panel);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(panel.get_state() == PIDCalibrationPanel::State::IDLE);
    for (const auto& line : client.gcode_script_history()) {
        CHECK(line.find("CALIBRATE") == std::string::npos);
    }
}

TEST_CASE_METHOD(XMLTestFixture, "Controls keypad keeps the printer's target after a refusal",
                 "[controls][indx][heat]") {
    EmptyHeadIndx indx;
    // Wired the way SubjectInitializer wires them at boot: the panel reads its
    // controller through the temperature service.
    auto controller = std::make_shared<helix::TemperatureController>(state(), &api());
    TemperatureService service(state(), &api());
    service.set_controller(controller.get());
    {
        ControlsPanel panel(state(), &api());
        panel.set_temp_control_panel(&service);

        helix::ui::ControlsPanelTestAccess::confirm_custom_nozzle(panel, 230.0f);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(helix::ui::ControlsPanelTestAccess::cached_extruder_target(panel) == 0);
    }
    helix::ui::UpdateQueue::instance().drain();
    service.set_controller(nullptr);
}
