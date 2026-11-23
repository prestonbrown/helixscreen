// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_clean_nozzle.cpp
 * @brief The Operations card's Clean button follows the CleanNozzle macro slot:
 * it shows only while the slot resolves to something the printer runs.
 */

#include "ui_panel_filament.h"

#include "../lvgl_ui_test_fixture.h"
#include "ams_state.h"
#include "printer_discovery.h"
#include "quick_action_buttons.h"
#include "standard_macros.h"
#include "tool_state.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Resolve every standard macro slot against a printer reporting @p macros.
void discover_macros(std::initializer_list<const char*> macros) {
    nlohmann::json objects = nlohmann::json::array({"extruder", "heater_bed", "gcode_move"});
    for (const char* m : macros) {
        objects.push_back(std::string("gcode_macro ") + m);
    }
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    StandardMacros::instance().init(hw, "");
}

/// The real FilamentPanel over the real filament_panel.xml.
struct CleanHarness {
    LVGLUITestFixture& fx;
    std::unique_ptr<FilamentPanel> panel;
    lv_obj_t* root = nullptr;

    explicit CleanHarness(LVGLUITestFixture& f) : fx(f) {
        ToolState::instance().init_subjects(true);
        AmsState::instance().init_subjects(true);
        AmsState::instance().clear_backends();
        panel = std::make_unique<FilamentPanel>(fx.state(), fx.api());
        panel->init_subjects();
        root = static_cast<lv_obj_t*>(lv_xml_create(fx.test_screen(), "filament_panel", nullptr));
        REQUIRE(root != nullptr);
        panel->setup(root, fx.test_screen());
        fx.process_lvgl(30);
    }

    ~CleanHarness() {
        panel.reset();
        AmsState::instance().clear_backends();
        AmsState::instance().deinit_subjects();
        ToolState::instance().deinit_subjects();
        StandardMacros::instance().reset();
    }

    [[nodiscard]] bool clean_hidden() {
        fx.process_lvgl(10);
        lv_obj_t* btn = lv_obj_find_by_name(root, "btn_clean_nozzle");
        REQUIRE(btn != nullptr);
        return lv_obj_is_hidden(btn);
    }
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "Filament panel: Clean shows only while the slot resolves",
                 "[filament][clean_nozzle]") {
    discover_macros({});
    CleanHarness h(*this);
    CHECK(h.clean_hidden());

    // A Klipper restart that brings a nozzle-clean macro re-resolves the slot.
    discover_macros({"CLEAN_NOZZLE"});
    CHECK_FALSE(h.clean_hidden());

    discover_macros({});
    CHECK(h.clean_hidden());
}

TEST_CASE("macro_button_state: ready, unavailable or hidden", "[filament][clean_nozzle]") {
    StandardMacroInfo info;
    CHECK(macro_button_state(info) == MacroButtonState::Hidden);

    info.missing_macro = "WIPE_IT";
    CHECK(macro_button_state(info) == MacroButtonState::Unavailable);

    info.detected_macro = "CLEAN_NOZZLE";
    CHECK(macro_button_state(info) == MacroButtonState::Ready);
}
