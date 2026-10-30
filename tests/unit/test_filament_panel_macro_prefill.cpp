// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_panel_macro_prefill.cpp
 * @brief The Filament panel hands its Load / Unload / Purge macros the nozzle
 *        temperature it already knows, and the parameter modal carries a prefill
 *        as a value that is sent, not as a hint.
 *
 * Run with: ./build/bin/helix-tests "[filament][prefill]"
 *
 * The temperature is the hotter of the live extruder target and the material the
 * panel would preheat for, never one at or below the printer's minimum extrusion
 * temperature, and never one above the hotend's max_temp. When the known values
 * cover every parameter the macro takes, the
 * macro runs with no dialog; otherwise the dialog opens with them typed in.
 */

#include "ui_ams_sidebar.h"
#include "ui_panel_filament.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/filament_panel_macro_harness.h"
#include "../test_helpers/filament_panel_test_access.h"
#include "../test_helpers/lane_material_backend.h"
#include "../test_helpers/load_filament_expression_default.h"
#include "../test_helpers/macro_param_modal_test_access.h"
#include "ams_state.h"
#include "app_globals.h"
#include "display_settings_manager.h"
#include "filament_database.h"
#include "filament_op_router.h"
#include "filament_op_slot_resolver.h"
#include "layout_manager.h"
#include "lvgl.h"
#include "macro_param_cache.h"
#include "macro_param_modal.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "post_op_cooldown_manager.h"
#include "preset_materials.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "standard_macros.h"
#include "tool_state.h"

#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using TA = helix::ui::FilamentPanelTestAccess;
using ParamValues = std::map<std::string, std::string>;
using Scripts = std::vector<std::string>;
using PrefillPanelHarness = helix::test::FilamentPanelMacroHarness;

namespace {

/// A macro that takes a nozzle temperature and one parameter the panel cannot know.
constexpr const char* kLoadWithLength = "{% set t = params.EXTRUDER_TEMP|default(220)|int %}\n"
                                        "{% set l = params.LENGTH|default(100)|float %}\n"
                                        "M109 S{t}\nG1 E{l} F300";

/// A load macro that also reads the temperature it purges at.
constexpr const char* kLoadWithPurgeTemp = "{% set t = params.EXTRUDER_TEMP|default(220)|int %}\n"
                                           "{% set p = params.PURGE_TEMP|default(250)|int %}\n"
                                           "M109 S{t}\nG1 E50 F300\nM109 S{p}\nG1 E30 F300";

/// Three parameters, the nozzle temperature in the middle.
constexpr const char* kTempBetweenTwo = "{% set l = params.LENGTH|default(100)|float %}\n"
                                        "{% set t = params.EXTRUDER_TEMP|default(220)|int %}\n"
                                        "{% set s = params.SPEED|default(5)|float %}\n"
                                        "M109 S{t}\nG1 E{l} F{s * 60}";

} // namespace

// =============================================================================
// Load
// =============================================================================

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Load runs a temperature-only macro with the live extruder target, no dialog",
                 "[filament][prefill]") {
    PrefillPanelHarness h;
    h.cache_macros({{"LOAD_FILAMENT", helix::test::kLoadFilamentExpressionDefault}});
    h.set_extruder_target(260.0);

    TA::execute_load(*h.panel);

    CHECK(h.prompt_count == 0);
    // Degrees, and only the parameter the macro reads.
    CHECK(h.sent_for("LOAD_FILAMENT") == Scripts{"LOAD_FILAMENT EXTRUDER_TEMP=260"});
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Load with the heater off sends the selected preset's temperature",
                 "[filament][prefill]") {
    PrefillPanelHarness h;
    h.cache_macros({{"LOAD_FILAMENT", helix::test::kLoadFilamentExpressionDefault}});
    h.set_extruder_target(0.0);
    TA::set_selected_material(*h.panel, 0);

    auto material = filament::find_material(helix::presets::name(0));
    REQUIRE(material);
    const int preset_temp = helix::ui::load_preheat_temp(*material);
    REQUIRE(preset_temp > 0);

    TA::execute_load(*h.panel);

    CHECK(h.prompt_count == 0);
    CHECK(h.sent_for("LOAD_FILAMENT") ==
          Scripts{"LOAD_FILAMENT EXTRUDER_TEMP=" + std::to_string(preset_temp)});
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Load with no temperature known opens the dialog with nothing filled in",
                 "[filament][prefill]") {
    PrefillPanelHarness h;
    h.cache_macros({{"LOAD_FILAMENT", helix::test::kLoadFilamentExpressionDefault}});
    h.set_extruder_target(0.0);
    TA::set_selected_material(*h.panel, -1);

    TA::execute_load(*h.panel);

    CHECK(h.prompt_count == 1);
    CHECK(h.prompted_macro == "LOAD_FILAMENT");
    CHECK(h.prompted_prefill.empty());
    CHECK(h.sent_for("LOAD_FILAMENT").empty());
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Load of a macro with a parameter nothing fills opens the dialog, temperature in",
                 "[filament][prefill]") {
    PrefillPanelHarness h;
    h.cache_macros({{"LOAD_FILAMENT", kLoadWithLength}});
    h.set_extruder_target(260.0);

    TA::execute_load(*h.panel);

    CHECK(h.prompt_count == 1);
    CHECK(h.prompted_prefill == ParamValues{{"EXTRUDER_TEMP", "260"}});
    CHECK(h.sent_for("LOAD_FILAMENT").empty());
}

// =============================================================================
// Unload and Purge take the same rule
// =============================================================================

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Unload hands a temperature-only macro the live extruder target",
                 "[filament][prefill]") {
    PrefillPanelHarness h;
    h.cache_macros({{"UNLOAD_FILAMENT",
                     "{% set t = params.TEMP|default(200)|int %}\nM109 S{t}\nG1 E-60 F600"}});
    h.set_extruder_target(250.0);

    TA::execute_unload(*h.panel);

    CHECK(h.prompt_count == 0);
    CHECK(h.sent_for("UNLOAD_FILAMENT") == Scripts{"UNLOAD_FILAMENT TEMP=250"});
}

TEST_CASE_METHOD(LVGLUITestFixture, "Purge sends PURGE_TEMP as a value, with no dialog",
                 "[filament][prefill]") {
    PrefillPanelHarness h;
    h.cache_macros(
        {{"PURGE", "{% set t = params.PURGE_TEMP|default(240)|int %}\nM109 S{t}\nG1 E30 F300"}});
    h.set_extruder_target(255.0);

    TA::execute_purge(*h.panel);

    CHECK(h.prompt_count == 0);
    CHECK(h.sent_for("PURGE") == Scripts{"PURGE PURGE_TEMP=255"});
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Purge with a parameter nothing fills asks through the shared prompt",
                 "[filament][prefill]") {
    PrefillPanelHarness h;
    h.cache_macros({{"PURGE", "{% set t = params.PURGE_TEMP|default(240)|int %}\n"
                              "{% set l = params.LENGTH|default(30)|float %}\n"
                              "M109 S{t}\nG1 E{l} F300"}});
    h.set_extruder_target(255.0);

    TA::execute_purge(*h.panel);

    CHECK(h.prompt_count == 1);
    CHECK(h.prompted_macro == "PURGE");
    CHECK(h.prompted_prefill == ParamValues{{"PURGE_TEMP", "255"}});
    CHECK(h.sent_for("PURGE").empty());
}

// =============================================================================
// Which temperature: the hotter known one, never below the extrusion minimum
// =============================================================================

TEST_CASE_METHOD(LVGLUITestFixture, "Load never sends a live target below the extrusion minimum",
                 "[filament][prefill]") {
    PrefillPanelHarness h;
    h.cache_macros({{"LOAD_FILAMENT", helix::test::kLoadFilamentExpressionDefault}});
    h.set_safety_limits(180.0);
    // A paused printer holds its nozzle at a standby temperature below the minimum.
    h.set_extruder_target(140.0);
    TA::set_selected_material(*h.panel, -1);

    TA::execute_load(*h.panel);

    CHECK(h.prompt_count == 1);
    CHECK(h.prompted_prefill.empty());
    CHECK(h.sent_for("LOAD_FILAMENT").empty());
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Load below the extrusion minimum sends the external spool's temperature instead",
                 "[filament][prefill]") {
    PrefillPanelHarness h;
    h.cache_macros({{"LOAD_FILAMENT", helix::test::kLoadFilamentExpressionDefault}});
    h.set_safety_limits(180.0);
    h.set_extruder_target(140.0);
    TA::set_selected_material(*h.panel, -1);
    h.set_external_spool(240);

    TA::execute_load(*h.panel);

    CHECK(h.prompt_count == 0);
    CHECK(h.sent_for("LOAD_FILAMENT") == Scripts{"LOAD_FILAMENT EXTRUDER_TEMP=240"});
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Load of a material hotter than the hotend allows opens the dialog",
                 "[filament][prefill]") {
    PrefillPanelHarness h;
    h.cache_macros({{"LOAD_FILAMENT", helix::test::kLoadFilamentExpressionDefault}});
    h.set_safety_limits(180.0, 280.0);
    h.set_extruder_target(0.0);
    TA::set_selected_material(*h.panel, -1);
    h.set_external_spool(290);

    TA::execute_load(*h.panel);

    CHECK(h.prompt_count == 1);
    CHECK(h.prompted_prefill.empty());
    CHECK(h.sent_for("LOAD_FILAMENT").empty());
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Load holds a fractional extrusion minimum to the next whole degree",
                 "[filament][prefill]") {
    PrefillPanelHarness h;
    h.cache_macros({{"LOAD_FILAMENT", helix::test::kLoadFilamentExpressionDefault}});
    // Klipper refuses 180 against a 180.5 minimum, so the whole-degree minimum is
    // 181, and a target at the minimum is not offered.
    h.set_safety_limits(180.5);
    h.set_extruder_target(181.0);
    TA::set_selected_material(*h.panel, -1);

    TA::execute_load(*h.panel);

    CHECK(h.prompt_count == 1);
    CHECK(h.prompted_prefill.empty());
    CHECK(h.sent_for("LOAD_FILAMENT").empty());
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Load sends the live target when it is hotter than the material",
                 "[filament][prefill]") {
    PrefillPanelHarness h;
    h.cache_macros({{"LOAD_FILAMENT", helix::test::kLoadFilamentExpressionDefault}});
    h.set_extruder_target(260.0);
    TA::set_selected_material(*h.panel, -1);
    h.set_external_spool(240);

    TA::execute_load(*h.panel);

    CHECK(h.prompt_count == 0);
    CHECK(h.sent_for("LOAD_FILAMENT") == Scripts{"LOAD_FILAMENT EXTRUDER_TEMP=260"});
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Load sends the material temperature when it is hotter than the live target",
                 "[filament][prefill]") {
    PrefillPanelHarness h;
    h.cache_macros({{"LOAD_FILAMENT", helix::test::kLoadFilamentExpressionDefault}});
    h.set_extruder_target(200.0);
    TA::set_selected_material(*h.panel, -1);
    h.set_external_spool(240);

    TA::execute_load(*h.panel);

    CHECK(h.prompt_count == 0);
    CHECK(h.sent_for("LOAD_FILAMENT") == Scripts{"LOAD_FILAMENT EXTRUDER_TEMP=240"});
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Load acting on an AMS lane sends that lane's material temperature",
                 "[filament][prefill]") {
    PrefillPanelHarness h(std::make_unique<helix::test::LaneMaterialBackend>(/*lane=*/2, 235));
    h.cache_macros({{"LOAD_FILAMENT", helix::test::kLoadFilamentExpressionDefault}});
    h.set_extruder_target(0.0);
    TA::set_selected_material(*h.panel, -1);

    TA::execute_load(*h.panel);

    CHECK(h.prompt_count == 0);
    CHECK(h.sent_for("LOAD_FILAMENT") == Scripts{"LOAD_FILAMENT EXTRUDER_TEMP=235"});
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Load of a macro that also reads PURGE_TEMP leaves that one to the dialog",
                 "[filament][prefill]") {
    PrefillPanelHarness h;
    h.cache_macros({{"LOAD_FILAMENT", kLoadWithPurgeTemp}});
    h.set_extruder_target(260.0);

    TA::execute_load(*h.panel);

    CHECK(h.prompt_count == 1);
    CHECK(h.prompted_prefill == ParamValues{{"EXTRUDER_TEMP", "260"}});
    CHECK(h.sent_for("LOAD_FILAMENT").empty());
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Purge with the heater off sends the external spool's temperature",
                 "[filament][prefill]") {
    PrefillPanelHarness h;
    h.cache_macros(
        {{"PURGE", "{% set t = params.PURGE_TEMP|default(240)|int %}\nM109 S{t}\nG1 E30 F300"}});
    h.set_extruder_target(0.0);
    TA::set_selected_material(*h.panel, -1);
    h.set_external_spool(245);

    TA::execute_purge(*h.panel);

    CHECK(h.prompt_count == 0);
    CHECK(h.sent_for("PURGE") == Scripts{"PURGE PURGE_TEMP=245"});
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Purge heats for the loaded lane's material, not the selected tool's slot",
                 "[filament][prefill]") {
    // Lane 1 is loaded and names the material; the selected tool maps to slot 3,
    // which names none.
    PrefillPanelHarness h(
        std::make_unique<helix::test::LaneMaterialBackend>(/*lane=*/1, 235, /*selected_slot=*/3));
    h.cache_macros(
        {{"PURGE", "{% set t = params.PURGE_TEMP|default(240)|int %}\nM109 S{t}\nG1 E30 F300"}});
    h.set_extruder_target(0.0);
    TA::set_selected_material(*h.panel, -1);

    TA::execute_purge(*h.panel);

    CHECK(h.prompt_count == 0);
    CHECK(h.sent_for("PURGE") == Scripts{"PURGE PURGE_TEMP=235"});
}

// =============================================================================
// Multi-extruder: the limits and target are the op's own extruder's
// =============================================================================

namespace {
constexpr const char* kPurgeTempOnly =
    "{% set t = params.PURGE_TEMP|default(240)|int %}\nM109 S{t}\nG1 E30 F300";
} // namespace

/// A panel over two hotends whose loaded lane 1 names a material heating to
/// @p nozzle_c and maps to @p lane_tool (T1 heats extruder1; -1 maps to none).
/// T0, on extruder, is the active tool.
struct TwoExtruderPurge {
    explicit TwoExtruderPurge(int nozzle_c, int lane_tool = 1)
        : h(std::make_unique<helix::test::LaneMaterialBackend>(/*lane=*/1, nozzle_c,
                                                               /*selected_slot=*/-1, lane_tool)) {
        h.use_two_extruders();
        h.cache_macros({{"PURGE", kPurgeTempOnly}});
        h.panel->set_limits(PrefillPanelHarness::two_extruder_limits());
        TA::set_selected_material(*h.panel, -1);
    }
    PrefillPanelHarness h;
};

TEST_CASE_METHOD(LVGLUITestFixture, "Purge on a lane mapped to T1 stays under extruder1's ceiling",
                 "[filament][prefill][multi_extruder]") {
    // 260 is within extruder's 300 and above extruder1's 250.
    TwoExtruderPurge t(/*nozzle_c=*/260);
    t.h.set_extruder_targets(0.0, 0.0);

    TA::execute_purge(*t.h.panel);

    CHECK(t.h.prompt_count == 1);
    CHECK(t.h.prompted_prefill.empty());
    CHECK(t.h.sent_for("PURGE").empty());
}

TEST_CASE_METHOD(LVGLUITestFixture, "Purge on a lane mapped to T1 stays above extruder1's floor",
                 "[filament][prefill][multi_extruder]") {
    // 190 is above extruder's 170 and below extruder1's 200.
    TwoExtruderPurge t(/*nozzle_c=*/190);
    t.h.set_extruder_targets(0.0, 0.0);

    TA::execute_purge(*t.h.panel);

    CHECK(t.h.prompt_count == 1);
    CHECK(t.h.prompted_prefill.empty());
    CHECK(t.h.sent_for("PURGE").empty());
}

TEST_CASE_METHOD(LVGLUITestFixture, "Purge on a lane mapped to T1 reads extruder1's target",
                 "[filament][prefill][multi_extruder]") {
    TwoExtruderPurge t(/*nozzle_c=*/210);
    t.h.set_extruder_targets(280.0, 230.0);

    TA::execute_purge(*t.h.panel);

    CHECK(t.h.prompt_count == 0);
    CHECK(t.h.sent_for("PURGE") == Scripts{"PURGE PURGE_TEMP=230"});
}

TEST_CASE_METHOD(LVGLUITestFixture, "Purge on a lane mapped to no tool uses the active extruder",
                 "[filament][prefill][multi_extruder]") {
    TwoExtruderPurge t(/*nozzle_c=*/210, /*lane_tool=*/-1);
    // 280 is within extruder's ceiling and above extruder1's.
    t.h.set_extruder_targets(280.0, 230.0);

    TA::execute_purge(*t.h.panel);

    CHECK(t.h.prompt_count == 0);
    CHECK(t.h.sent_for("PURGE") == Scripts{"PURGE PURGE_TEMP=280"});
}

/// The AMS sidebar loading lane 1, which names a material heating to @p nozzle_c
/// and maps to T1, on the same two hotends.
struct TwoExtruderSidebarLoad {
    explicit TwoExtruderSidebarLoad(int nozzle_c)
        : h(std::make_unique<helix::test::LaneMaterialBackend>(/*lane=*/1, nozzle_c,
                                                               /*selected_slot=*/-1,
                                                               /*lane_tool=*/1)) {
        h.use_two_extruders();
        h.cache_macros({{"LOAD_FILAMENT", helix::test::kLoadFilamentExpressionDefault}});
        h.api.set_safety_limits(PrefillPanelHarness::two_extruder_limits());
    }
    /// The sidebar runs its macro through its lifetime token, so it outlives the drain.
    void load() {
        sidebar.handle_load_with_preheat(1);
        helix::ui::UpdateQueue::instance().drain();
    }
    PrefillPanelHarness h;
    helix::ui::AmsOperationSidebar sidebar{h.state};
};

TEST_CASE_METHOD(LVGLUITestFixture, "The AMS sidebar prefills a T1 lane's load from extruder1",
                 "[filament][prefill][multi_extruder][ams]") {
    SECTION("its target") {
        TwoExtruderSidebarLoad t(/*nozzle_c=*/210);
        t.h.set_extruder_targets(280.0, 230.0);
        t.load();
        CHECK(t.h.prompt_count == 0);
        CHECK(t.h.sent_for("LOAD_FILAMENT") == Scripts{"LOAD_FILAMENT EXTRUDER_TEMP=230"});
    }
    SECTION("its ceiling") {
        TwoExtruderSidebarLoad t(/*nozzle_c=*/260);
        t.h.set_extruder_targets(0.0, 0.0);
        t.load();
        CHECK(t.h.prompt_count == 1);
        CHECK(t.h.prompted_prefill.empty());
        CHECK(t.h.sent_for("LOAD_FILAMENT").empty());
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "Purge offers its temperature only as PURGE_TEMP",
                 "[filament][prefill]") {
    PrefillPanelHarness h;
    h.cache_macros(
        {{"PURGE", "{% set t = params.TEMP|default(240)|int %}\nM109 S{t}\nG1 E30 F300"}});
    h.set_extruder_target(255.0);

    TA::execute_purge(*h.panel);

    CHECK(h.prompt_count == 1);
    CHECK(h.prompted_prefill.empty());
    CHECK(h.sent_for("PURGE").empty());
}

// =============================================================================
// MacroParamModal: a prefill is the field's text
// =============================================================================

namespace {

class ParamModalFixture : public LVGLUITestFixture {
  public:
    ParamModalFixture() {
        // Synchronous hide, so a dismissed modal is gone by the next drain.
        prev_animations_ = helix::DisplaySettingsManager::instance().get_animations_enabled();
        helix::DisplaySettingsManager::instance().set_animations_enabled(false);
    }

    ~ParamModalFixture() override {
        // A failed assertion can leave the modal up with its static instance
        // pointer aimed at this soon-dead object.
        if (modal.dialog()) {
            helix::MacroParamModal::cancel_cb(nullptr);
        }
        process_lvgl(20);
        helix::DisplaySettingsManager::instance().set_animations_enabled(prev_animations_);
    }

    /// Show the modal for @p params with @p prefill typed in.
    void show(const std::vector<helix::MacroParam>& params, const ParamValues& prefill) {
        modal.show_for_macro(
            lv_screen_active(), "LOAD_FILAMENT", params,
            [this](const helix::MacroParamResult& result) {
                ++runs;
                sent = result;
            },
            prefill);
        REQUIRE(modal.dialog() != nullptr);
    }

    /// Show the modal for the expression-default LOAD_FILAMENT and return its one field.
    lv_obj_t* show_load_filament(const ParamValues& prefill) {
        auto params = helix::parse_macro_params(helix::test::kLoadFilamentExpressionDefault);
        REQUIRE(params.size() == 1);
        show(params, prefill);
        lv_obj_t* field = lv_obj_find_by_name(modal.dialog(), "field_input");
        REQUIRE(field != nullptr);
        return field;
    }

    /// Every field's text, in the order the fields are laid out.
    std::vector<std::string> field_texts() const {
        std::vector<std::string> texts;
        lv_obj_t* list = lv_obj_find_by_name(modal.dialog(), "param_list");
        REQUIRE(list != nullptr);
        const uint32_t count = lv_obj_get_child_count(list);
        for (uint32_t i = 0; i < count; ++i) {
            lv_obj_t* input =
                lv_obj_find_by_name(lv_obj_get_child(list, static_cast<int32_t>(i)), "field_input");
            REQUIRE(input != nullptr);
            texts.emplace_back(lv_textarea_get_text(input));
        }
        return texts;
    }

    helix::MacroParamModal modal;
    int runs = 0;
    helix::MacroParamResult sent;

  private:
    bool prev_animations_ = true;
};

/// Removes the form_field component for its lifetime, so no parameter field can be
/// built, and registers it again from its file on the way out. A process that has not
/// registered it yet (this case running first in its shard) already has no such field,
/// and is left that way.
struct FormFieldUnregistered {
    FormFieldUnregistered()
        : was_registered_(lv_xml_component_unregister("form_field") == LV_RESULT_OK) {}
    ~FormFieldUnregistered() {
        if (!was_registered_)
            return;
        const std::string path =
            "A:" + helix::LayoutManager::instance().resolve_xml_path("form_field.xml");
        lv_xml_register_component_from_file(path.c_str());
    }
    FormFieldUnregistered(const FormFieldUnregistered&) = delete;
    FormFieldUnregistered& operator=(const FormFieldUnregistered&) = delete;

  private:
    bool was_registered_;
};

} // namespace

TEST_CASE_METHOD(ParamModalFixture, "A prefilled parameter is typed into its field and sent on Run",
                 "[filament][prefill][macro_params]") {
    lv_obj_t* field = show_load_filament({{"EXTRUDER_TEMP", "260"}});

    CHECK(std::string(lv_textarea_get_text(field)) == "260");
    // What a cleared field says: where the value comes from, not template source.
    CHECK(std::string(lv_textarea_get_placeholder_text(field)) == lv_tr("Printer default"));

    helix::MacroParamModal::run_cb(nullptr);
    process_lvgl(20);

    CHECK(runs == 1);
    CHECK(sent.params == ParamValues{{"EXTRUDER_TEMP", "260"}});
}

TEST_CASE_METHOD(ParamModalFixture, "A cleared prefill leaves the parameter to the macro",
                 "[filament][prefill][macro_params]") {
    lv_obj_t* field = show_load_filament({{"EXTRUDER_TEMP", "260"}});
    lv_textarea_set_text(field, "");

    helix::MacroParamModal::run_cb(nullptr);
    process_lvgl(20);

    CHECK(runs == 1);
    CHECK(sent.params.empty());
}

TEST_CASE_METHOD(ParamModalFixture,
                 "A prefill lands in its own parameter's field and leaves the others empty",
                 "[filament][prefill][macro_params]") {
    const auto params = helix::parse_macro_params(kTempBetweenTwo);
    REQUIRE(params.size() == 3);
    REQUIRE(params[1].name == "EXTRUDER_TEMP");

    show(params, {{"EXTRUDER_TEMP", "260"}});

    CHECK(field_texts() == std::vector<std::string>{"", "260", ""});

    helix::MacroParamModal::run_cb(nullptr);
    process_lvgl(20);

    CHECK(runs == 1);
    CHECK(sent.params == ParamValues{{"EXTRUDER_TEMP", "260"}});
}

TEST_CASE_METHOD(ParamModalFixture,
                 "A field that cannot be built still holds its parameter's place",
                 "[filament][prefill][macro_params]") {
    const auto params = helix::parse_macro_params(kTempBetweenTwo);
    REQUIRE(params.size() == 3);

    {
        FormFieldUnregistered no_form_field;
        show(params, {{"EXTRUDER_TEMP", "260"}});
    }

    // Values are paired with parameters by position, so a missing field must not
    // shift the ones after it onto the wrong name.
    CHECK(helix::MacroParamModalTestAccess::field_slot_count(modal) == params.size());
}
