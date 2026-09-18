// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/filament_panel_macro_harness.h"
#include "../test_helpers/filament_panel_test_access.h"
#include "../test_helpers/preheat_widget_test_access.h"
#include "../test_helpers/scoped_shared_resource.h"
#include "filament_catalog.h"
#include "macro_executor.h"
#include "material_settings_manager.h"
#include "preheat_widget.h"
#include "preset_materials.h"
#include "safety_settings_manager.h"
#include "settings_manager.h"
#include "temperature_controller.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

using PanelAccess = helix::ui::FilamentPanelTestAccess;
using WidgetAccess = helix::PreheatWidgetTestAccess;
using Scripts = std::vector<std::string>;
using Harness = helix::test::FilamentPanelMacroHarness;

// Material settings are process-wide; Config writes are sandboxed by the base
// fixture. Establish and clean the baseline even when a REQUIRE unwinds a case.
struct MaterialSettingsScope {
    MaterialSettingsScope() {
        helix::MaterialSettingsManager::instance().init();
        reset();
    }
    ~MaterialSettingsScope() {
        reset();
    }
    static void reset() {
        auto& manager = helix::MaterialSettingsManager::instance();
        const auto overrides = manager.get_all_overrides();
        for (const auto& [name, unused] : overrides) {
            (void)unused;
            manager.clear_override(name);
        }
        manager.reset_preset_materials();
    }
};

enum class PreheatAction { FILAMENT_PRESET, SPOOL_PRESET, HOME_WIDGET };

filament::MaterialOverride abs_override(std::optional<std::string> macro,
                                        std::optional<bool> handles_heating = std::nullopt) {
    filament::MaterialOverride result;
    result.nozzle_min = 250;
    result.nozzle_max = 250;
    result.bed_temp = 105;
    result.chamber_temp = 45;
    result.preheat_macro = std::move(macro);
    result.macro_handles_heating = handles_heating;
    return result;
}

// Target 0 makes the Home widget a preheat action, not Cool Down. Both the
// physical nozzle and its remembered prior target still exceed the preset.
void feed_hot_nozzle(Harness& h) {
    h.state.update_from_status({{"extruder", {{"temperature", 269.0}, {"target", 269.0}}}});
    h.state.update_from_status({{"extruder", {{"temperature", 269.0}, {"target", 0.0}}}});
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(h.state.get_active_extruder_last_nonzero_target() == Catch::Approx(269.0));
    REQUIRE(lv_subject_get_int(h.state.get_active_extruder_temp_subject()) == 2690);
    REQUIRE(lv_subject_get_int(h.state.get_active_extruder_target_subject()) == 0);
    h.client.clear_gcode_script_history();
}

void press(PreheatAction action, Harness& h) {
    switch (action) {
    case PreheatAction::FILAMENT_PRESET:
        PanelAccess::handle_preset_button(*h.panel, 2);
        break;
    case PreheatAction::SPOOL_PRESET: {
        // The dynamic spool preset is offered when its material has no fixed button.
        helix::MaterialSettingsManager::instance().set_preset_material(2, "ASA");
        helix::SlotInfo spool;
        spool.material = "ABS";
        helix::AmsState::instance().set_external_spool_info_in_memory(spool);
        PanelAccess::update_spool_preset(*h.panel);
        PanelAccess::handle_spool_preset_button(*h.panel);
        break;
    }
    case PreheatAction::HOME_WIDGET: {
        helix::PreheatWidget widget(h.state);
        widget.set_config({{"material_index", 2}});
        WidgetAccess::handle_apply(widget);
        break;
    }
    }
}

const Scripts ABS_TARGETS{"SET_HEATER_TEMPERATURE HEATER=extruder TARGET=250",
                          "SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=105"};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "Material preheat buttons let the configured macro own heating",
                 "[material_preheat][preheat][filament]") {
    const auto action = GENERATE(PreheatAction::FILAMENT_PRESET, PreheatAction::SPOOL_PRESET,
                                 PreheatAction::HOME_WIDGET);
    const auto handles_heating = GENERATE(std::optional<bool>{true}, std::optional<bool>{});
    CAPTURE(static_cast<int>(action), handles_heating);
    MaterialSettingsScope settings;
    Harness h;
    helix_test::ScopedSharedResource<helix::TemperatureController> controller(
        std::make_shared<helix::TemperatureController>(h.state, &h.api));
    helix::MaterialSettingsManager::instance().set_override(
        "ABS", abs_override("HEAT_ABS", handles_heating));
    feed_hot_nozzle(h);

    press(action, h);

    CHECK(h.client.gcode_script_history() == Scripts{"HEAT_ABS"});
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Material preheat buttons send exact targets before an additive macro",
                 "[material_preheat][preheat][filament]") {
    const auto action = GENERATE(PreheatAction::FILAMENT_PRESET, PreheatAction::SPOOL_PRESET,
                                 PreheatAction::HOME_WIDGET);
    CAPTURE(static_cast<int>(action));
    MaterialSettingsScope settings;
    Harness h;
    helix_test::ScopedSharedResource<helix::TemperatureController> controller(
        std::make_shared<helix::TemperatureController>(h.state, &h.api));
    helix::MaterialSettingsManager::instance().set_override("ABS", abs_override("HEAT_ABS", false));
    feed_hot_nozzle(h);

    press(action, h);

    Scripts expected = ABS_TARGETS;
    expected.push_back("HEAT_ABS");
    CHECK(h.client.gcode_script_history() == expected);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Material preheat without a macro lowers a hot nozzle to the chosen target",
                 "[material_preheat][preheat][filament]") {
    const auto action = GENERATE(PreheatAction::FILAMENT_PRESET, PreheatAction::SPOOL_PRESET,
                                 PreheatAction::HOME_WIDGET);
    const auto macro = GENERATE(std::optional<std::string>{}, std::optional<std::string>{""});
    CAPTURE(static_cast<int>(action), macro);
    MaterialSettingsScope settings;
    Harness h;
    helix_test::ScopedSharedResource<helix::TemperatureController> controller(
        std::make_shared<helix::TemperatureController>(h.state, &h.api));
    helix::MaterialSettingsManager::instance().set_override("ABS", abs_override(macro));
    feed_hot_nozzle(h);

    press(action, h);

    CHECK(h.client.gcode_script_history() == ABS_TARGETS);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Reassigned branded presets keep their temperatures and material macro",
                 "[material_preheat][preheat][presets]") {
    MaterialSettingsScope settings;
    Harness h;
    helix_test::ScopedSharedResource<helix::TemperatureController> controller(
        std::make_shared<helix::TemperatureController>(h.state, &h.api));
    auto& manager = helix::MaterialSettingsManager::instance();
    manager.set_override("ABS", abs_override("WRONG_ABS_MACRO", false));
    manager.set_override("ASA", abs_override("HEAT_ASA", false));
    manager.set_preset_material(2, "ASA");
    helix::printer::EffectiveFilament product;
    product.id = "test:asa-product";
    product.brand = "Test Brand";
    product.name = "ASA Product";
    product.type = "ASA";
    product.nozzle_recommended = 255;
    product.bed_temp = 95;
    manager.set_preset_filament(2, product);
    REQUIRE(helix::presets::name(2) == "ASA");
    REQUIRE(manager.get_preset_filament(2).has_value());
    REQUIRE(manager.get_preset_filament(2)->is_branded());

    press(PreheatAction::FILAMENT_PRESET, h);

    CHECK(h.client.gcode_script_history() ==
          Scripts{"SET_HEATER_TEMPERATURE HEATER=extruder TARGET=255",
                  "SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=95", "HEAT_ASA"});
}

TEST_CASE_METHOD(LVGLUITestFixture, "Home preheat uses the reassigned material's targets and macro",
                 "[material_preheat][preheat][presets]") {
    MaterialSettingsScope settings;
    Harness h;
    helix_test::ScopedSharedResource<helix::TemperatureController> controller(
        std::make_shared<helix::TemperatureController>(h.state, &h.api));
    auto& manager = helix::MaterialSettingsManager::instance();
    manager.set_override("ABS", abs_override("WRONG_ABS_MACRO", false));
    manager.set_override("ASA", abs_override("HEAT_ASA", false));
    manager.set_preset_material(2, "ASA");
    REQUIRE(helix::presets::name(2) == "ASA");

    press(PreheatAction::HOME_WIDGET, h);

    CHECK(h.client.gcode_script_history() ==
          Scripts{"SET_HEATER_TEMPERATURE HEATER=extruder TARGET=250",
                  "SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=105", "HEAT_ASA"});
}

TEST_CASE_METHOD(LVGLUITestFixture, "Spool preheat resolves a case-insensitive material's macro",
                 "[material_preheat][preheat][filament]") {
    MaterialSettingsScope settings;
    Harness h;
    helix_test::ScopedSharedResource<helix::TemperatureController> controller(
        std::make_shared<helix::TemperatureController>(h.state, &h.api));
    helix::MaterialSettingsManager::instance().set_override("ABS", abs_override("HEAT_ABS", true));
    helix::SlotInfo spool;
    spool.material = "abs";
    helix::MaterialSettingsManager::instance().set_preset_material(2, "ASA");
    helix::AmsState::instance().set_external_spool_info_in_memory(spool);
    PanelAccess::update_spool_preset(*h.panel);

    PanelAccess::handle_spool_preset_button(*h.panel);

    CHECK(h.client.gcode_script_history() == Scripts{"HEAT_ABS"});
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Filament material preheat includes its discovered chamber before the macro",
                 "[material_preheat][preheat][chamber]") {
    MaterialSettingsScope settings;
    Harness h;
    helix_test::ScopedSharedResource<helix::TemperatureController> controller(
        std::make_shared<helix::TemperatureController>(h.state, &h.api));
    helix::MaterialSettingsManager::instance().set_override("ABS", abs_override("HEAT_ABS", false));
    helix::SettingsManager::instance().set_chamber_heater_assignment("auto");
    helix::PrinterDiscovery hardware;
    hardware.parse_objects(
        nlohmann::json{"extruder", "heater_bed", "heater_generic chamber_heater"});
    h.state.set_hardware(hardware);
    REQUIRE(controller.get().resolved_name(helix::HeaterType::Chamber) ==
            "heater_generic chamber_heater");

    PanelAccess::handle_preset_button(*h.panel, 2);

    CHECK(h.client.gcode_script_history() ==
          Scripts{"SET_HEATER_TEMPERATURE HEATER=extruder TARGET=250",
                  "SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=105",
                  "SET_HEATER_TEMPERATURE HEATER=chamber_heater TARGET=45", "HEAT_ABS"});
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Home additive preheat honors selected or all tools before running its macro",
                 "[material_preheat][preheat][panel_widget]") {
    MaterialSettingsScope settings;
    Harness h;
    h.use_two_extruders();
    REQUIRE(ToolState::instance().has_multiple_extruders());
    helix_test::ScopedSharedResource<helix::TemperatureController> controller(
        std::make_shared<helix::TemperatureController>(h.state, &h.api));
    helix::MaterialSettingsManager::instance().set_override("ABS", abs_override("HEAT_ABS", false));
    helix::PreheatWidget widget(h.state);
    widget.set_config({{"material_index", 2}});
    const int tool_target = GENERATE(1, -1);
    CAPTURE(tool_target);
    WidgetAccess::set_tool_target(widget, tool_target);

    WidgetAccess::handle_apply(widget);

    Scripts expected{"SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=250",
                     "SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=105", "HEAT_ABS"};
    if (tool_target == -1) {
        expected.insert(expected.begin(), "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=250");
    }
    CHECK(h.client.gcode_script_history() == expected);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Load and unload retain the previous filament's purge temperature",
                 "[material_preheat][preheat][filament][swap_preheat]") {
    const bool load = GENERATE(true, false);
    CAPTURE(load);
    MaterialSettingsScope settings;
    Harness h;
    helix_test::ScopedSharedResource<helix::TemperatureController> controller(
        std::make_shared<helix::TemperatureController>(h.state, &h.api));
    helix::MaterialSettingsManager::instance().set_override("ABS", abs_override("HEAT_ABS", true));
    helix::SafetySettingsManager::instance().set_allow_cold_extrude(false);
    h.set_safety_limits(170.0);
    h.state.update_from_status({{"extruder", {{"temperature", 25.0}, {"target", 269.0}}},
                                {"toolhead", {{"homed_axes", "xyz"}}}});
    h.state.update_from_status({{"extruder", {{"temperature", 25.0}, {"target", 0.0}}}});
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(h.state.get_active_extruder_last_nonzero_target() == Catch::Approx(269.0));
    PanelAccess::set_selected_material(*h.panel, 2);
    h.client.clear_gcode_script_history();

    if (load) {
        PanelAccess::handle_load_button(*h.panel);
    } else {
        PanelAccess::handle_unload_button(*h.panel);
    }

    CHECK(h.client.gcode_script_history() ==
          Scripts{"SET_HEATER_TEMPERATURE HEATER=extruder TARGET=269"});
}

TEST_CASE_METHOD(LVGLUITestFixture, "Material preheat without a printer API sends no temperatures",
                 "[material_preheat][preheat]") {
    const bool has_macro = GENERATE(true, false);
    CAPTURE(has_macro);
    MaterialSettingsScope settings;
    helix::MaterialSettingsManager::instance().set_override(
        "ABS",
        abs_override(has_macro ? std::optional<std::string>{"HEAT_ABS"} : std::nullopt, false));
    helix::PrinterDiscovery hardware;
    int temperature_sends = 0;

    helix::execute_material_preheat(
        nullptr, "ABS", [&] { ++temperature_sends; }, "[MaterialPreheatTest]", hardware);

    CHECK(temperature_sends == 0);
}
