// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_manual_pull_prompt.h"
#include "ui_nav_manager.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/ams_state_test_access.h"
#include "../test_helpers/filament_panel_macro_harness.h"
#include "../test_helpers/filament_panel_test_access.h"
#include "../test_helpers/preheat_widget_test_access.h"
#include "../test_helpers/scoped_shared_resource.h"
#include "../test_helpers/toolchanger_test_helper.h"
#include "../ui_test_utils.h"
#include "ams_backend_indx.h"
#include "ams_state.h"
#include "filament_catalog.h"
#include "filament_op_heater.h"
#include "filament_op_router.h"
#include "macro_executor.h"
#include "macro_param_cache.h"
#include "material_settings_manager.h"
#include "preheat_widget.h"
#include "preset_materials.h"
#include "printer_discovery.h"
#include "safety_settings_manager.h"
#include "settings_manager.h"
#include "standard_macros.h"
#include "temperature_controller.h"

#include <algorithm>
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

// The printer defines the material macros these cases assign.
void discover_macros(Harness& h, std::vector<std::string> objects = {"extruder", "heater_bed"}) {
    objects.insert(objects.end(), {"gcode_macro HEAT_ABS", "gcode_macro HEAT_ASA"});
    helix::PrinterDiscovery hardware;
    hardware.parse_objects(nlohmann::json(objects));
    h.state.set_hardware(hardware);
}

// Target 0 makes the Home widget a preheat action, not Cool Down. Both the
// physical nozzle and its remembered prior target still exceed the preset.
void feed_hot_nozzle(Harness& h) {
    h.state.update_from_status({{"extruder", {{"temperature", 269.0}, {"target", 269.0}}}});
    h.state.update_from_status({{"extruder", {{"temperature", 269.0}, {"target", 0.0}}}});
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(h.state.temperature_state().get_active_extruder_last_nonzero_target() ==
            Catch::Approx(269.0));
    REQUIRE(lv_subject_get_int(h.state.temperature_state().get_active_extruder_temp_subject()) ==
            2690);
    REQUIRE(lv_subject_get_int(h.state.temperature_state().get_active_extruder_target_subject()) ==
            0);
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
        helix::AmsStateTestAccess::seed_external_spool(spool);
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

const Scripts kAbsTargets{"SET_HEATER_TEMPERATURE HEATER=extruder TARGET=250",
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
    discover_macros(h);
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
    discover_macros(h);
    helix_test::ScopedSharedResource<helix::TemperatureController> controller(
        std::make_shared<helix::TemperatureController>(h.state, &h.api));
    helix::MaterialSettingsManager::instance().set_override("ABS", abs_override("HEAT_ABS", false));
    feed_hot_nozzle(h);

    press(action, h);

    Scripts expected = kAbsTargets;
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
    discover_macros(h);
    helix_test::ScopedSharedResource<helix::TemperatureController> controller(
        std::make_shared<helix::TemperatureController>(h.state, &h.api));
    helix::MaterialSettingsManager::instance().set_override("ABS", abs_override(macro));
    feed_hot_nozzle(h);

    press(action, h);

    CHECK(h.client.gcode_script_history() == kAbsTargets);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Material preheat heats to the preset when this printer lacks the macro",
                 "[material_preheat][preheat][filament]") {
    const auto action = GENERATE(PreheatAction::FILAMENT_PRESET, PreheatAction::SPOOL_PRESET,
                                 PreheatAction::HOME_WIDGET);
    const auto handles_heating = GENERATE(std::optional<bool>{true}, std::optional<bool>{false});
    CAPTURE(static_cast<int>(action), handles_heating);
    MaterialSettingsScope settings;
    Harness h;
    discover_macros(h);
    helix_test::ScopedSharedResource<helix::TemperatureController> controller(
        std::make_shared<helix::TemperatureController>(h.state, &h.api));
    helix::MaterialSettingsManager::instance().set_override(
        "ABS", abs_override("OTHER_PRINTER_HEAT_ABS", handles_heating));
    feed_hot_nozzle(h);

    press(action, h);

    CHECK(h.client.gcode_script_history() == kAbsTargets);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Reassigned branded presets keep their temperatures and material macro",
                 "[material_preheat][preheat][presets]") {
    MaterialSettingsScope settings;
    Harness h;
    discover_macros(h);
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
    discover_macros(h);
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
    discover_macros(h);
    helix_test::ScopedSharedResource<helix::TemperatureController> controller(
        std::make_shared<helix::TemperatureController>(h.state, &h.api));
    helix::MaterialSettingsManager::instance().set_override("ABS", abs_override("HEAT_ABS", true));
    helix::SlotInfo spool;
    spool.material = "abs";
    helix::MaterialSettingsManager::instance().set_preset_material(2, "ASA");
    helix::AmsStateTestAccess::seed_external_spool(spool);
    PanelAccess::update_spool_preset(*h.panel);

    PanelAccess::handle_spool_preset_button(*h.panel);

    CHECK(h.client.gcode_script_history() == Scripts{"HEAT_ABS"});
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Filament material preheat includes its discovered chamber before the macro",
                 "[material_preheat][preheat][chamber]") {
    MaterialSettingsScope settings;
    Harness h;
    discover_macros(h);
    helix_test::ScopedSharedResource<helix::TemperatureController> controller(
        std::make_shared<helix::TemperatureController>(h.state, &h.api));
    helix::MaterialSettingsManager::instance().set_override("ABS", abs_override("HEAT_ABS", false));
    helix::SettingsManager::instance().set_chamber_heater_assignment("auto");
    discover_macros(h, {"extruder", "heater_bed", "heater_generic chamber_heater"});
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
    discover_macros(h);
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
    discover_macros(h);
    helix_test::ScopedSharedResource<helix::TemperatureController> controller(
        std::make_shared<helix::TemperatureController>(h.state, &h.api));
    helix_test::ScopedSharedResource<helix::ui::FilamentOpHeater> heater(
        std::make_shared<helix::ui::FilamentOpHeater>(h.state));
    helix::MaterialSettingsManager::instance().set_override("ABS", abs_override("HEAT_ABS", true));
    helix::SafetySettingsManager::instance().set_allow_cold_extrude(false);
    h.set_safety_limits(170.0);
    h.state.update_from_status({{"extruder", {{"temperature", 25.0}, {"target", 269.0}}},
                                {"toolhead", {{"homed_axes", "xyz"}}}});
    h.state.update_from_status({{"extruder", {{"temperature", 25.0}, {"target", 0.0}}}});
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(h.state.temperature_state().get_active_extruder_last_nonzero_target() ==
            Catch::Approx(269.0));
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
    using Mode = helix::MaterialPreheatMode;
    const auto mode = GENERATE(Mode::TEMPERATURES, Mode::TEMPERATURES_THEN_MACRO, Mode::MACRO_ONLY,
                               Mode::MACRO_MISSING);
    CAPTURE(static_cast<int>(mode));
    helix::PrinterDiscovery hardware;
    int temperature_sends = 0;

    helix::execute_material_preheat(
        nullptr, {mode, "HEAT_ABS"}, [&] { ++temperature_sends; }, "[MaterialPreheatTest]",
        hardware);

    CHECK(temperature_sends == 0);
}

namespace {

helix::PrinterDiscovery printer_with_macro(const char* macro) {
    helix::PrinterDiscovery hardware;
    hardware.parse_objects(nlohmann::json{"extruder", std::string("gcode_macro ") + macro});
    return hardware;
}

} // namespace

TEST_CASE("plan_material_preheat sends temperatures when no macro is assigned",
          "[material_preheat][preheat]") {
    const auto hardware = printer_with_macro("HEAT_ABS");
    filament::MaterialOverride override;
    override.macro_handles_heating = true;
    const auto macro = GENERATE(std::optional<std::string>{}, std::optional<std::string>{""});
    CAPTURE(macro);
    override.preheat_macro = macro;

    for (const auto* candidate : {static_cast<const filament::MaterialOverride*>(nullptr),
                                  static_cast<const filament::MaterialOverride*>(&override)}) {
        const auto plan = helix::plan_material_preheat(candidate, hardware);
        CHECK(plan.mode == helix::MaterialPreheatMode::TEMPERATURES);
        CHECK(plan.macro.empty());
    }
}

TEST_CASE("plan_material_preheat lets a discovered macro heat unless it is additive",
          "[material_preheat][preheat]") {
    const auto hardware = printer_with_macro("HEAT_ABS");
    filament::MaterialOverride override;
    override.preheat_macro = "heat_abs"; // discovery matches macros case-insensitively

    override.macro_handles_heating = std::nullopt;
    auto plan = helix::plan_material_preheat(&override, hardware);
    CHECK(plan.mode == helix::MaterialPreheatMode::MACRO_ONLY);
    CHECK(plan.macro == "heat_abs");

    override.macro_handles_heating = true;
    CHECK(helix::plan_material_preheat(&override, hardware).mode ==
          helix::MaterialPreheatMode::MACRO_ONLY);

    override.macro_handles_heating = false;
    plan = helix::plan_material_preheat(&override, hardware);
    CHECK(plan.mode == helix::MaterialPreheatMode::TEMPERATURES_THEN_MACRO);
    CHECK(plan.macro == "heat_abs");
}

TEST_CASE("plan_material_preheat flags a macro this printer does not define",
          "[material_preheat][preheat]") {
    const auto hardware = printer_with_macro("HEAT_PLA");
    filament::MaterialOverride override;
    override.preheat_macro = "HEAT_ABS";
    const auto handles_heating =
        GENERATE(std::optional<bool>{}, std::optional<bool>{true}, std::optional<bool>{false});
    CAPTURE(handles_heating);
    override.macro_handles_heating = handles_heating;

    const auto plan = helix::plan_material_preheat(&override, hardware);

    CHECK(plan.mode == helix::MaterialPreheatMode::MACRO_MISSING);
    CHECK(plan.macro == "HEAT_ABS");
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Extrude, retract and purge heat a cold nozzle before they move filament",
                 "[material_preheat][preheat][filament][heater]") {
    enum class Button { Extrude, Retract, Purge };
    const Button button = GENERATE(Button::Extrude, Button::Retract, Button::Purge);
    CAPTURE(static_cast<int>(button));
    Harness h;
    // No purge macro: the panel's purge is its built-in extrude sequence.
    helix::PrinterDiscovery bare;
    bare.parse_objects(nlohmann::json{"extruder"});
    StandardMacros::instance().reset();
    StandardMacros::instance().init(bare);
    helix_test::ScopedSharedResource<helix::TemperatureController> controller(
        std::make_shared<helix::TemperatureController>(h.state, &h.api));
    helix_test::ScopedSharedResource<helix::ui::FilamentOpHeater> heater(
        std::make_shared<helix::ui::FilamentOpHeater>(h.state));
    helix::SafetySettingsManager::instance().set_allow_cold_extrude(false);
    h.state.update_from_status({{"extruder", {{"temperature", 25.0}, {"target", 0.0}}}});
    helix::ui::UpdateQueue::instance().drain();
    h.client.clear_gcode_script_history();

    switch (button) {
    case Button::Extrude:
        PanelAccess::handle_extrude_button(*h.panel);
        break;
    case Button::Retract:
        PanelAccess::handle_retract_button(*h.panel);
        break;
    case Button::Purge:
        PanelAccess::handle_purge_button(*h.panel);
        break;
    }
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(heater.get().pending());
    const auto& before = h.client.gcode_script_history();
    REQUIRE(before.size() == 1);
    CHECK(before[0].rfind("SET_HEATER_TEMPERATURE HEATER=extruder TARGET=", 0) == 0);

    h.state.update_from_status({{"extruder", {{"temperature", 220.0}, {"target", 220.0}}}});
    helix::ui::UpdateQueue::instance().drain();
    CHECK_FALSE(heater.get().pending());
    CHECK(h.client.gcode_script_history().size() > 1); // the op itself went out
}

TEST_CASE_METHOD(LVGLUITestFixture, "A panel Purge replaces another surface's pending load",
                 "[material_preheat][preheat][filament][heater]") {
    Harness h;
    helix::PrinterDiscovery bare;
    bare.parse_objects(nlohmann::json{"extruder"});
    StandardMacros::instance().reset();
    StandardMacros::instance().init(bare);
    helix_test::ScopedSharedResource<helix::TemperatureController> controller(
        std::make_shared<helix::TemperatureController>(h.state, &h.api));
    helix_test::ScopedSharedResource<helix::ui::FilamentOpHeater> heater(
        std::make_shared<helix::ui::FilamentOpHeater>(h.state));
    helix::SafetySettingsManager::instance().set_allow_cold_extrude(false);
    h.state.update_from_status({{"extruder", {{"temperature", 25.0}, {"target", 0.0}}}});
    helix::ui::UpdateQueue::instance().drain();

    // The home tile's load is waiting on the nozzle.
    helix::ui::execute_filament_load(nullptr, -1, "[FilamentSensorWidget]");
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(heater.get().pending());

    PanelAccess::handle_purge_button(*h.panel);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(heater.get().pending()); // replaced, not withdrawn
    h.client.clear_gcode_script_history();

    // Hot enough for whatever material the panel's preset names.
    h.state.update_from_status({{"extruder", {{"temperature", 290.0}, {"target", 290.0}}}});
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE_FALSE(heater.get().pending());
    // The mock records each line of a script on its own: the purge fallback's
    // extrude is G1 E50, the load fallback's first move G1 E56.
    const auto& sent = h.client.gcode_script_history();
    const auto has = [&sent](const std::string& line) {
        return std::find(sent.begin(), sent.end(), line) != sent.end();
    };
    CHECK(has("G1 E50 F600"));
    CHECK_FALSE(has("G1 E56 F1200"));
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "A panel load that waited raises no prompt once the user has left the panel",
                 "[material_preheat][preheat][filament][heater]") {
    Harness h;
    discover_macros(h, {"extruder", "heater_bed", "gcode_macro LOAD_FILAMENT"});
    helix::PrinterDiscovery hardware;
    hardware.parse_objects(nlohmann::json{"extruder", "gcode_macro LOAD_FILAMENT"});
    StandardMacros::instance().reset();
    StandardMacros::instance().init(hardware);
    nlohmann::json config;
    // LENGTH is a parameter nothing prefills, so a prompting dispatch would ask.
    config["gcode_macro LOAD_FILAMENT"]["gcode"] =
        "{% set t = params.TEMP|default(220)|int %}\n{% set l = params.LENGTH|default(50)|int "
        "%}\nM109 S{t}\nG1 E{l} F300";
    helix::MacroParamCache::instance().populate_from_configfile(config, {"LOAD_FILAMENT"});
    int prompts = 0;
    helix::ui::set_filament_param_prompter([&prompts](const std::string&,
                                                      const helix::CachedMacroInfo&,
                                                      const std::map<std::string, std::string>&,
                                                      helix::MacroExecuteCallback) { ++prompts; });
    helix_test::ScopedSharedResource<helix::TemperatureController> controller(
        std::make_shared<helix::TemperatureController>(h.state, &h.api));
    helix_test::ScopedSharedResource<helix::ui::FilamentOpHeater> heater(
        std::make_shared<helix::ui::FilamentOpHeater>(h.state));
    helix::SafetySettingsManager::instance().set_allow_cold_extrude(false);
    h.state.update_from_status({{"extruder", {{"temperature", 25.0}, {"target", 0.0}}},
                                {"toolhead", {{"homed_axes", "xyz"}}}});
    helix::ui::UpdateQueue::instance().drain();
    // The tap happened on the panel; the user is elsewhere when the nozzle is hot.
    REQUIRE(NavigationManager::instance().get_active() != helix::PanelId::Filament);

    PanelAccess::execute_load(*h.panel);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(heater.get().pending());

    h.state.update_from_status({{"extruder", {{"temperature", 290.0}, {"target", 290.0}}}});
    helix::ui::UpdateQueue::instance().drain();
    CHECK_FALSE(heater.get().pending());
    CHECK(prompts == 0);
    helix::ui::set_filament_param_prompter({});
    helix::MacroParamCache::instance().clear();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "A panel Unload the heater does not run leaves no manual-pull prompt armed",
                 "[material_preheat][preheat][filament][heater]") {
    // Extrude is heating; the user taps Unload, which the heater refuses. The
    // manual-pull prompt armed for that unload would otherwise fire on the next
    // unrelated toolhead-sensor edge, or on the next unload's end.
    Harness h;
    helix::PrinterDiscovery bare;
    bare.parse_objects(nlohmann::json{"extruder"});
    StandardMacros::instance().reset();
    StandardMacros::instance().init(bare);
    helix_test::ScopedSharedResource<helix::TemperatureController> controller(
        std::make_shared<helix::TemperatureController>(h.state, &h.api));
    helix_test::ScopedSharedResource<helix::ui::FilamentOpHeater> heater(
        std::make_shared<helix::ui::FilamentOpHeater>(h.state));
    helix::SafetySettingsManager::instance().set_allow_cold_extrude(false);
    h.state.update_from_status({{"extruder", {{"temperature", 25.0}, {"target", 0.0}}}});
    helix::ui::UpdateQueue::instance().drain();

    PanelAccess::handle_extrude_button(*h.panel);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(heater.get().pending());

    PanelAccess::execute_unload(*h.panel);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(heater.get().pending()); // refused: the extrude still heats

    std::vector<std::string> toasts;
    helix::ui::set_test_notification_info_hook(
        [&toasts](const std::string& msg) { toasts.push_back(msg); });
    helix::ui::manual_pull_unload_finished(); // speaks only when armed
    helix::ui::set_test_notification_info_hook(nullptr);
    CHECK(toasts.empty());
}

namespace {

/// A shared-heater backend with no tool on the head: it refuses nozzle heat.
std::unique_ptr<helix::AmsBackend> empty_head_indx() {
    auto indx = std::make_unique<helix::test::GcodeCaptureBackend<helix::AmsBackendIndx>>();
    helix::PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json::array(
        {"indx", "save_variables", "extruder", "gcode_macro TOOL_POSITIONS",
         "gcode_macro _TOOLCHANGE_VARS", "gcode_macro CHANGE_TOOL", "gcode_macro PARK_TOOL"}));
    indx->set_discovery(hw);
    indx->feed(nlohmann::json{{"save_variables", {{"variables", {{"active_tool", -1}}}}}});
    return indx;
}

int shown_nozzle_target(Harness& h) {
    int current = -1;
    int target = -1;
    h.panel->get_temp(&current, &target);
    return target;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "A preset whose nozzle half is refused shows the printer's own target",
                 "[material_preheat][preheat][indx][heat]") {
    MaterialSettingsScope settings;
    Harness h(empty_head_indx());
    discover_macros(h);
    helix_test::ScopedSharedResource<helix::TemperatureController> controller(
        std::make_shared<helix::TemperatureController>(h.state, &h.api));
    REQUIRE_FALSE(helix::AmsState::instance().nozzle_heat_blocked_reason().empty());

    PanelAccess::handle_preset_button(*h.panel, 0);
    helix::ui::UpdateQueue::instance().drain();

    // The bed half still applies; the nozzle half never reached the printer.
    CHECK(h.client.gcode_script_history() ==
          Scripts{"SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=60"});
    CHECK(shown_nozzle_target(h) == 0);
    helix::AmsState::instance().clear_backends();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "A refused keypad or spool-preset nozzle target is not shown as set",
                 "[material_preheat][preheat][indx][heat]") {
    MaterialSettingsScope settings;
    Harness h(empty_head_indx());
    discover_macros(h);
    helix_test::ScopedSharedResource<helix::TemperatureController> controller(
        std::make_shared<helix::TemperatureController>(h.state, &h.api));

    SECTION("keypad") {
        PanelAccess::confirm_custom_nozzle(*h.panel, 230.0f);
    }
    SECTION("spool preset") {
        helix::MaterialSettingsManager::instance().set_preset_material(2, "ASA");
        helix::SlotInfo spool;
        spool.material = "ABS";
        helix::AmsStateTestAccess::seed_external_spool(spool);
        PanelAccess::update_spool_preset(*h.panel);
        PanelAccess::handle_spool_preset_button(*h.panel);
    }
    helix::ui::UpdateQueue::instance().drain();
    CHECK(shown_nozzle_target(h) == 0);
    for (const auto& line : h.client.gcode_script_history()) {
        CHECK(line.find("HEATER=extruder") == std::string::npos);
    }
    helix::AmsState::instance().clear_backends();
}
