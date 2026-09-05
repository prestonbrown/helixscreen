// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_manage_row_controls.cpp
 * @brief The filament card's manage row carries two mutually exclusive
 * controls, and tool count alone decides which one.
 *
 * The tool selector belongs to every multi-tool printer: the options come from
 * ToolState, and handle_extruder_changed() issues a gcode Tn when no AMS
 * backend claims the tool. The Manage button navigates to the AMS panel, which
 * needs a backend, so it is the single-tool affordance (#1350).
 */

#include "ui_panel_filament.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/filament_panel_test_access.h"
#include "ams_state.h"
#include "ams_types.h"
#include "printer_discovery.h"
#include "tool_state.h"

#include <lvgl.h>
#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;
using TA = helix::ui::FilamentPanelTestAccess;

namespace {

/// Builds the real FilamentPanel over the real filament_panel.xml so the
/// visibility assertions read the same widgets production shows.
struct ManageRowHarness {
    LVGLUITestFixture& fx;
    std::unique_ptr<FilamentPanel> panel;
    lv_obj_t* root = nullptr;

    /// @param extruder_heaters Klipper extruder objects the printer reports.
    ///        Two or more with no [tool N] object is a plain multi-extruder
    ///        machine, which ToolState turns into one tool per heater.
    /// @param ams_type Value of the ams_type subject; 0 means no backend.
    ManageRowHarness(LVGLUITestFixture& f, const std::vector<std::string>& extruder_heaters,
                     AmsType ams_type)
        : fx(f) {
        ToolState::instance().init_subjects(true);
        AmsState::instance().init_subjects(true);
        AmsState::instance().clear_backends();

        nlohmann::json objects = nlohmann::json::array();
        for (const auto& h : extruder_heaters) {
            objects.push_back(h);
        }
        objects.push_back("heater_bed");
        objects.push_back("fan");
        objects.push_back("gcode_move");

        helix::PrinterDiscovery hw;
        hw.parse_objects(objects);
        ToolState::instance().init_tools(hw);

        lv_subject_set_int(AmsState::instance().get_ams_type_subject(),
                           static_cast<int>(ams_type));

        panel = std::make_unique<FilamentPanel>(fx.state(), fx.api());
        panel->init_subjects();

        root = static_cast<lv_obj_t*>(lv_xml_create(fx.test_screen(), "filament_panel", nullptr));
        REQUIRE(root != nullptr);
        panel->setup(root, fx.test_screen());
        fx.process_lvgl(30);

        TA::populate_extruder_dropdown(*panel);
    }

    ~ManageRowHarness() {
        panel.reset();
        AmsState::instance().clear_backends();
    }

    [[nodiscard]] bool hidden(const char* name) const {
        lv_obj_t* obj = lv_obj_find_by_name(root, name);
        REQUIRE(obj != nullptr);
        return lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Filament manage row: multi-tool without AMS gets the tool selector",
                 "[filament][ui][tool]") {
    ManageRowHarness h(*this, {"extruder", "extruder1"}, AmsType::NONE);

    REQUIRE(ToolState::instance().is_multi_tool());

    // The row is the container both controls live in; it must be on screen for
    // either assertion below to describe what the user sees.
    REQUIRE_FALSE(h.hidden("ams_manage_row"));

    // Manage navigates to the AMS panel, which returns early with no backend.
    CHECK(h.hidden("btn_manage_slots"));

    // The tools exist in ToolState whether or not a backend claimed them.
    CHECK_FALSE(h.hidden("extruder_selector_group"));
}

TEST_CASE_METHOD(LVGLUITestFixture, "Filament manage row: single-tool with AMS gets Manage",
                 "[filament][ui][tool]") {
    ManageRowHarness h(*this, {"extruder"}, AmsType::AFC);

    REQUIRE_FALSE(ToolState::instance().is_multi_tool());
    REQUIRE_FALSE(h.hidden("ams_manage_row"));
    CHECK_FALSE(h.hidden("btn_manage_slots"));
    CHECK(h.hidden("extruder_selector_group"));
}

TEST_CASE_METHOD(LVGLUITestFixture, "Filament manage row: multi-tool with AMS gets the selector",
                 "[filament][ui][tool]") {
    ManageRowHarness h(*this, {"extruder", "extruder1"}, AmsType::AFC);

    REQUIRE(ToolState::instance().is_multi_tool());
    REQUIRE_FALSE(h.hidden("ams_manage_row"));
    CHECK(h.hidden("btn_manage_slots"));
    CHECK_FALSE(h.hidden("extruder_selector_group"));
}
