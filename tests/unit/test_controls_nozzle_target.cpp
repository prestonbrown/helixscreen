// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_controls_nozzle_target.cpp
 * @brief The Controls nozzle row shows and edits the tool chosen on it, and
 *        follows the carriage when a tool change happens anywhere.
 */

#include "ui_panel_controls.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/controls_panel_test_access.h"
#include "ams_state.h"
#include "core/update_queue.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "temperature_controller.h"
#include "temperature_service.h"
#include "tool_state.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::ToolState;
using TA = helix::ui::ControlsPanelTestAccess;

namespace {

struct NozzleRowHarness {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    MoonrakerAPI api{client, state};
    std::shared_ptr<helix::TemperatureController> controller =
        std::make_shared<helix::TemperatureController>(state, &api);
    std::unique_ptr<TemperatureService> service;
    std::unique_ptr<ControlsPanel> panel;
    std::vector<std::string> extruders;

    explicit NozzleRowHarness(std::vector<std::string> names) : extruders(std::move(names)) {
        ToolState::instance().init_subjects(true);
        helix::AmsState::instance().init_subjects(true);
        helix::AmsState::instance().clear_backends();
        state.init_subjects(false);
        state.temperature_state().init_extruders(extruders);
        state.set_klippy_state_sync(helix::KlippyState::READY);
        nlohmann::json objects = nlohmann::json::array();
        for (const auto& e : extruders) {
            objects.push_back(e);
        }
        objects.push_back("heater_bed");
        helix::PrinterDiscovery hw;
        hw.parse_objects(objects);
        state.set_hardware(hw);
        ToolState::instance().init_tools(hw);
        mount(extruders.size() > 1 ? 1 : 0);

        service = std::make_unique<TemperatureService>(state, &api);
        service->set_controller(controller.get());
        panel = std::make_unique<ControlsPanel>(state, &api);
        panel->set_temp_control_panel(service.get());
        TA::init_and_observe(*panel);
        helix::ui::UpdateQueue::instance().drain();
        client.clear_gcode_script_history();
    }

    ~NozzleRowHarness() {
        helix::ui::UpdateQueue::instance().drain();
        panel.reset();
        service->set_controller(nullptr);
        service.reset();
        helix::ui::UpdateQueue::instance().drain();
    }

    void mount(size_t tool) {
        ToolState::instance().update_from_status({{"toolhead", {{"extruder", extruders[tool]}}}});
        state.update_from_status({{"toolhead", {{"extruder", extruders[tool]}}}});
        helix::ui::UpdateQueue::instance().drain();
    }

    [[nodiscard]] bool sent(const std::string& needle) const {
        const auto& h = client.gcode_script_history();
        return std::any_of(h.begin(), h.end(), [&](const std::string& l) {
            return l.find(needle) != std::string::npos;
        });
    }
};

int subject_int(const char* name) {
    lv_subject_t* s = lv_xml_get_subject(nullptr, name);
    REQUIRE(s != nullptr);
    return lv_subject_get_int(s);
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "Controls nozzle row shows and edits the chosen tool",
                 "[controls][target_tool]") {
    NozzleRowHarness h({"extruder", "extruder1", "extruder2"});
    CHECK(subject_int("controls_nozzle_multi_head") == 1);
    h.state.update_from_status(
        {{"extruder1", {{"target", 180.0}}}, {"extruder2", {{"target", 230.0}}}});
    helix::ui::UpdateQueue::instance().drain();
    CHECK(TA::cached_extruder_target(*h.panel) == 1800);

    h.panel->select_nozzle_tool(2);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(TA::cached_extruder_target(*h.panel) == 2300);
    CHECK(subject_int("controls_nozzle_tgt_deci") == 2300);

    TA::confirm_custom_nozzle(*h.panel, 220.0f);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(h.sent("SET_HEATER_TEMPERATURE HEATER=extruder2 TARGET=220"));
    CHECK_FALSE(h.sent("HEATER=extruder1 "));

    SECTION("a tool change moves the row to the mounted tool") {
        h.mount(0);
        CHECK(TA::nozzle_tool(*h.panel) == 0);
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "Controls nozzle row on one head edits it as before",
                 "[controls][target_tool]") {
    NozzleRowHarness h({"extruder"});
    CHECK(subject_int("controls_nozzle_multi_head") == 0);
    TA::confirm_custom_nozzle(*h.panel, 215.0f);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(h.sent("SET_HEATER_TEMPERATURE HEATER=extruder TARGET=215"));
}
