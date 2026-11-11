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

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/filament_panel_test_access.h"
#include "../test_helpers/modal_answer.h"
#include "../test_helpers/scoped_shared_resource.h"
#include "ams_state.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "material_settings_manager.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "safety_settings_manager.h"
#include "target_tool_selection.h"
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

    [[nodiscard]] bool sent_line(const std::string& line) const {
        const auto& h = client.gcode_script_history();
        return std::find(h.begin(), h.end(), line) != h.end();
    }

    [[nodiscard]] int action() const {
        lv_subject_t* subject = lv_xml_get_subject(nullptr, "filament_target_tool_action");
        REQUIRE(subject != nullptr);
        return lv_subject_get_int(subject);
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

namespace {

/// Leaves "Confirm tool changes" as the test found it.
struct ConfirmSetting {
    bool saved = helix::SafetySettingsManager::instance().get_tool_change_require_confirmation();
    explicit ConfirmSetting(bool on) {
        helix::SafetySettingsManager::instance().set_tool_change_require_confirmation(on);
    }
    ~ConfirmSetting() {
        helix::SafetySettingsManager::instance().set_tool_change_require_confirmation(saved);
    }
};

void settle(TargetToolHarness& h, uint32_t ms = 50) {
    helix::ui::UpdateQueue::instance().drain();
    h.fx.process_lvgl(ms);
    helix::ui::UpdateQueue::instance().drain();
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Filament panel: extrude picks the target up and waits until it is mounted",
                 "[filament][target_tool]") {
    TargetToolHarness h(*this);
    ConfirmSetting confirm(false);
    // Hot enough that extrusion needs no preheat.
    h.state.update_from_status(
        {{"extruder1", {{"temperature", 220.0}}}, {"extruder2", {{"temperature", 220.0}}}});
    settle(h);
    h.choose(2);
    h.client.clear_gcode_script_history();

    TA::handle_extrude_button(*h.panel);
    settle(h);
    CHECK(h.sent_line("T2"));
    CHECK_FALSE(h.sent("G1 E"));

    SECTION("the move follows the tool's arrival") {
        h.mount(2);
        settle(h);
        CHECK(h.sent("G1 E"));
    }
    SECTION("no arrival: the move is dropped") {
        h.fx.process_lvgl(helix::ui::kToolArrivalTimeoutMs + 500);
        settle(h);
        CHECK_FALSE(h.sent("G1 E"));
        h.mount(2);
        settle(h);
        CHECK_FALSE(h.sent("G1 E"));
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "Filament panel: a declined pickup sends no extrusion",
                 "[filament][target_tool]") {
    TargetToolHarness h(*this);
    ConfirmSetting confirm(true);
    h.state.update_from_status({{"extruder2", {{"temperature", 220.0}}}});
    settle(h);
    h.choose(2);
    h.client.clear_gcode_script_history();

    TA::handle_extrude_button(*h.panel);
    settle(h);
    REQUIRE(Modal::get_top() != nullptr);
    Modal::hide(Modal::get_top());
    h.fx.process_lvgl(500);
    settle(h);
    CHECK_FALSE(h.sent_line("T2"));
    CHECK_FALSE(h.sent("G1 E"));
}

TEST_CASE_METHOD(LVGLUITestFixture, "Filament panel: extruding the mounted tool changes no tool",
                 "[filament][target_tool]") {
    TargetToolHarness h(*this);
    ConfirmSetting confirm(false);
    h.state.update_from_status({{"extruder1", {{"temperature", 220.0}}}});
    settle(h);
    h.client.clear_gcode_script_history();
    TA::handle_extrude_button(*h.panel);
    settle(h);
    CHECK(h.sent("G1 E"));
    CHECK_FALSE(h.sent_line("T1"));
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Filament panel: with no filament system, Load picks the target up first",
                 "[filament][target_tool]") {
    TargetToolHarness h(*this);
    ConfirmSetting confirm(false);
    h.choose(2);
    h.client.clear_gcode_script_history();
    TA::handle_load_button(*h.panel);
    settle(h);
    REQUIRE_FALSE(h.client.gcode_script_history().empty());
    CHECK(h.client.gcode_script_history().front() == "T2");
}

TEST_CASE_METHOD(LVGLUITestFixture, "Filament panel: the Pick up button follows the target",
                 "[filament][target_tool]") {
    TargetToolHarness h(*this);
    ConfirmSetting confirm(false);
    // Mounted tool targeted; a plain multi-extruder cannot park.
    CHECK(h.action() == static_cast<int>(helix::ui::TargetToolAction::None));
    h.choose(2);
    CHECK(h.action() == static_cast<int>(helix::ui::TargetToolAction::PickUp));

    h.client.clear_gcode_script_history();
    TA::handle_target_tool_action(*h.panel);
    settle(h);
    CHECK(h.sent_line("T2"));
}

// With "Confirm tool changes" on, Pick up waits on the dialog; a change that
// does not happen leaves the panel aimed where it was.
TEST_CASE_METHOD(LVGLUITestFixture, "Filament panel: Pick up asks first",
                 "[filament][target_tool][confirm]") {
    TargetToolHarness h(*this);
    ConfirmSetting confirm(true);
    h.choose(2);
    h.client.clear_gcode_script_history();
    TA::handle_target_tool_action(*h.panel);
    settle(h);
    CHECK_FALSE(h.sent_line("T2"));
    REQUIRE(ModalStack::instance().top_dialog() != nullptr);

    SECTION("confirming changes tool") {
        helix::test::answer_top_modal("btn_primary");
        settle(h);
        CHECK(h.sent_line("T2"));
    }
    SECTION("cancelling changes nothing and keeps the aim") {
        helix::test::answer_top_modal("btn_secondary");
        settle(h);
        CHECK_FALSE(h.sent_line("T2"));
        CHECK(TA::target_tool(*h.panel) == 2);
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "Filament panel: Pick up is refused while printing",
                 "[filament][target_tool][confirm]") {
    TargetToolHarness h(*this);
    ConfirmSetting confirm(true);
    h.choose(2);
    h.state.update_from_status({{"print_stats", {{"state", "printing"}}}});
    settle(h);
    h.client.clear_gcode_script_history();
    TA::handle_target_tool_action(*h.panel);
    settle(h);
    CHECK_FALSE(h.sent_line("T2"));
    CHECK(ModalStack::instance().stack_empty());
}
