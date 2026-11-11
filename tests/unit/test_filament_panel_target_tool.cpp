// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_panel_target_tool.cpp
 * @brief The Filament panel aims at the tool chosen in its selector: the
 *        selector never changes tools, and the temperature card, keypad and
 *        presets follow the chosen tool's extruder.
 */

#include "ui_modal.h"
#include "ui_panel_filament.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/filament_panel_test_access.h"
#include "../test_helpers/scoped_shared_resource.h"
#include "ams_state.h"
#include "app_globals.h"
#include "material_settings_manager.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "temperature_controller.h"
#include "tool_state.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::ToolState;
using TA = helix::ui::FilamentPanelTestAccess;

namespace {

/// A plain three-extruder printer (one tool per heater, no AMS backend) with the
/// real FilamentPanel built over the real filament_panel.xml.
struct TargetToolHarness {
    LVGLUITestFixture& fx;
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    MoonrakerAPI api{client, state};
    std::unique_ptr<FilamentPanel> panel;
    lv_obj_t* root = nullptr;
    IMoonrakerAPI* previous_api = nullptr;
    helix_test::ScopedSharedResource<helix::TemperatureController> controller{
        std::make_shared<helix::TemperatureController>(state, &api)};

    explicit TargetToolHarness(LVGLUITestFixture& f) : fx(f) {
        helix::MaterialSettingsManager::instance().init();
        ToolState::instance().init_subjects(true);
        helix::AmsState::instance().init_subjects(true);
        helix::AmsState::instance().clear_backends();
        state.init_subjects(false);
        state.temperature_state().init_extruders({"extruder", "extruder1", "extruder2"});
        state.set_klippy_state_sync(helix::KlippyState::READY);

        helix::PrinterDiscovery hw;
        hw.parse_objects(nlohmann::json{"extruder", "extruder1", "extruder2", "heater_bed"});
        state.set_hardware(hw);
        ToolState::instance().init_tools(hw);
        mount(1);

        previous_api = get_moonraker_api();
        set_moonraker_api(&api);
        panel = std::make_unique<FilamentPanel>(state, &api);
        panel->init_subjects();
        root = static_cast<lv_obj_t*>(lv_xml_create(fx.test_screen(), "filament_panel", nullptr));
        REQUIRE(root != nullptr);
        panel->setup(root, fx.test_screen());
        fx.process_lvgl(30);
        TA::populate_extruder_dropdown(*panel);
        client.clear_gcode_script_history();
    }

    ~TargetToolHarness() {
        helix::ui::UpdateQueue::instance().drain();
        panel.reset();
        set_moonraker_api(previous_api);
        helix::AmsState::instance().clear_backends();
        helix::ui::UpdateQueue::instance().drain();
    }

    void mount(int tool) {
        static const char* kNames[] = {"extruder", "extruder1", "extruder2"};
        ToolState::instance().update_from_status({{"toolhead", {{"extruder", kNames[tool]}}}});
        state.update_from_status({{"toolhead", {{"extruder", kNames[tool]}}}});
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(ToolState::instance().active_tool_index() == tool);
    }

    void choose(int tool) {
        lv_obj_t* dd = TA::extruder_dropdown(*panel);
        REQUIRE(dd != nullptr);
        lv_dropdown_set_selected(dd, static_cast<uint32_t>(tool));
        TA::handle_extruder_changed(*panel);
        helix::ui::UpdateQueue::instance().drain();
    }

    [[nodiscard]] bool sent(const std::string& needle) const {
        const auto& h = client.gcode_script_history();
        return std::any_of(h.begin(), h.end(), [&](const std::string& l) {
            return l.find(needle) != std::string::npos;
        });
    }
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Filament panel: choosing a tool aims the panel without a tool change",
                 "[filament][target_tool]") {
    TargetToolHarness h(*this);
    h.choose(2);
    CHECK(TA::target_tool(*h.panel) == 2);
    CHECK(h.client.gcode_script_history().empty());
    // Not merely deferred behind the tool-change confirmation either.
    CHECK(Modal::get_top() == nullptr);
    CHECK(ToolState::instance().active_tool_index() == 1);
}

TEST_CASE_METHOD(LVGLUITestFixture, "Filament panel: the temperature card shows the chosen tool",
                 "[filament][target_tool]") {
    TargetToolHarness h(*this);
    h.state.update_from_status(
        {{"extruder1", {{"target", 180.0}}}, {"extruder2", {{"target", 230.0}}}});
    helix::ui::UpdateQueue::instance().drain();
    CHECK(TA::nozzle_target(*h.panel) == 180);
    h.choose(2);
    h.fx.process_lvgl(20);
    CHECK(TA::nozzle_target(*h.panel) == 230);
}

TEST_CASE_METHOD(LVGLUITestFixture, "Filament panel: the keypad heats the chosen tool",
                 "[filament][target_tool]") {
    TargetToolHarness h(*this);
    h.choose(2);
    TA::confirm_custom_nozzle(*h.panel, 215.0f);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(h.sent("SET_HEATER_TEMPERATURE HEATER=extruder2 TARGET=215"));
    CHECK_FALSE(h.sent("HEATER=extruder1 "));
}

TEST_CASE_METHOD(LVGLUITestFixture, "Filament panel: a material preset heats the chosen tool",
                 "[filament][target_tool]") {
    TargetToolHarness h(*this);
    h.choose(2);
    TA::handle_preset_button(*h.panel, 0);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(h.sent("HEATER=extruder2 TARGET="));
    CHECK(h.sent("HEATER=heater_bed TARGET="));
    CHECK_FALSE(h.sent("HEATER=extruder1 "));
}

TEST_CASE_METHOD(LVGLUITestFixture, "Filament panel: a tool change moves the selection",
                 "[filament][target_tool]") {
    TargetToolHarness h(*this);
    h.choose(2);
    h.mount(0);
    CHECK(TA::target_tool(*h.panel) == 0);
    lv_obj_t* dd = TA::extruder_dropdown(*h.panel);
    CHECK(lv_dropdown_get_selected(dd) == 0);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Filament panel: a pick past a shrunk tool list falls back to T0",
                 "[filament][target_tool]") {
    TargetToolHarness h(*this);
    h.choose(2);
    helix::PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json{"extruder", "extruder1", "heater_bed"});
    ToolState::instance().init_tools(hw);
    helix::ui::UpdateQueue::instance().drain();
    h.fx.process_lvgl(20);
    CHECK(TA::target_tool(*h.panel) < 2);
}
