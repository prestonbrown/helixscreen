// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_clean_nozzle.cpp
 * @brief The Operations card in the real Filament panel: Clean follows the
 * CleanNozzle macro slot, and the extrude speed dropdown stays filled wherever
 * the layout moves it.
 */

#include "ui_panel_filament.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/scoped_int_subject.h"
#include "ams_state.h"
#include "core/update_queue.h"
#include "printer_discovery.h"
#include "quick_action_buttons.h"
#include "standard_macros.h"
#include "tool_state.h"

#include <memory>
#include <string>

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

// A rotation rebuilds the Operations card, and with it the <if> pair that puts
// the speed dropdown in the heading or beside the heads. A head-count change
// afterwards moves the dropdown again; the panel must fill the one it lands in.
TEST_CASE_METHOD(
    LVGLUITestFixture,
    "Filament panel: the speed dropdown stays filled after a rotation and a head change",
    "[filament][clean_nozzle]") {
    discover_macros({});
    helix::test::ScopedIntSubject single_head("target_tool_multi_head", 0);
    CleanHarness h(*this);
    lv_subject_t* portrait = lv_xml_get_subject(nullptr, "ui_is_portrait");
    REQUIRE(portrait != nullptr);
    const int was_portrait = lv_subject_get_int(portrait);
    lv_subject_set_int(portrait, was_portrait ? 0 : 1);
    process_lvgl(10);
    lv_subject_set_int(portrait, was_portrait);
    process_lvgl(10);

    lv_subject_set_int(lv_xml_get_subject(nullptr, "target_tool_multi_head"), 1);
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(10);

    lv_obj_t* dd = lv_obj_find_by_name(h.root, "extrude_speed_dropdown");
    REQUIRE(dd != nullptr);
    CHECK(lv_dropdown_get_option_count(dd) > 0);
    const char* text = lv_dropdown_get_text(dd);
    REQUIRE(text != nullptr);
    CHECK(std::string(text).find("mm/s") != std::string::npos);
}
