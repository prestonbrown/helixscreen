// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_filament_panel_extrude_speed.cpp
 * @brief The Filament panel's speed dropdown edits the extrude_speed setting
 *        Machine Limits also sets, and Extrude / Retract move at that speed.
 */

#include "ui_panel_filament.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/filament_panel_macro_harness.h"
#include "../test_helpers/filament_panel_test_access.h"
#include "core/update_queue.h"
#include "safety_settings_manager.h"
#include "settings_manager.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {

using TA = helix::ui::FilamentPanelTestAccess;

/// SettingsManager's subjects live before the panel observes them.
struct SettingsLive {
    SettingsLive() {
        helix::SettingsManager::instance().init_subjects();
    }
};

/// The panel built from its XML over the macro harness's recording client.
struct SpeedHarness {
    LVGLUITestFixture& fx;
    SettingsLive settings_live;
    helix::test::FilamentPanelMacroHarness h;
    lv_obj_t* root = nullptr;
    int saved_speed = helix::SettingsManager::instance().get_extrude_speed();
    bool saved_cold = helix::SafetySettingsManager::instance().get_allow_cold_extrude();

    explicit SpeedHarness(LVGLUITestFixture& f) : fx(f) {
        helix::SettingsManager::instance().set_extrude_speed(5);
        helix::SafetySettingsManager::instance().set_allow_cold_extrude(true);
        root = static_cast<lv_obj_t*>(lv_xml_create(fx.test_screen(), "filament_panel", nullptr));
        REQUIRE(root != nullptr);
        h.panel->setup(root, fx.test_screen());
        settle();
        h.client.clear_gcode_script_history();
    }

    ~SpeedHarness() {
        if (root) {
            lv_obj_delete(root);
        }
        fx.process_lvgl(10);
        helix::SettingsManager::instance().set_extrude_speed(saved_speed);
        helix::SafetySettingsManager::instance().set_allow_cold_extrude(saved_cold);
    }

    void settle() {
        helix::ui::UpdateQueue::instance().drain();
        fx.process_lvgl(20);
        helix::ui::UpdateQueue::instance().drain();
    }

    lv_obj_t* dropdown() {
        lv_obj_t* dd = TA::extrude_speed_dropdown(*h.panel);
        REQUIRE(dd != nullptr);
        return dd;
    }

    /// The user picks option @p index from the open list.
    void choose(uint32_t index) {
        lv_dropdown_set_selected(dropdown(), index);
        TA::handle_extrude_speed_select(*h.panel,
                                        static_cast<int>(lv_dropdown_get_selected(dropdown())));
        settle();
    }

    [[nodiscard]] std::string shown() {
        const char* text = lv_dropdown_get_text(dropdown());
        return text ? text : "";
    }

    [[nodiscard]] bool sent(const std::string& needle) const {
        const auto& hist = h.client.gcode_script_history();
        return std::any_of(hist.begin(), hist.end(), [&](const std::string& l) {
            return l.find(needle) != std::string::npos;
        });
    }
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "Filament panel: the speed dropdown sets the extrude speed",
                 "[filament][extrude_speed]") {
    SpeedHarness s(*this);
    CHECK(s.shown() == "5 mm/s");

    // Options follow SettingsManager::kExtrudeSpeedPresets: 2, 5, 10, 20 mm/s.
    s.choose(2);
    CHECK(helix::SettingsManager::instance().get_extrude_speed() == 10);
    CHECK(s.shown() == "10 mm/s");
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Filament panel: a speed set in Machine Limits shows on the dropdown",
                 "[filament][extrude_speed]") {
    SpeedHarness s(*this);
    helix::SettingsManager::instance().set_extrude_speed(7); // not a preset
    s.settle();
    CHECK(s.shown() == "7 mm/s");
}

TEST_CASE_METHOD(LVGLUITestFixture, "Filament panel: extrude and retract move at the chosen speed",
                 "[filament][extrude_speed]") {
    SpeedHarness s(*this);
    s.choose(3); // 20 mm/s

    SECTION("extrude") {
        TA::handle_extrude_button(*s.h.panel);
        s.settle();
        CHECK(s.sent("G1 E10 F1200"));
    }
    SECTION("retract") {
        TA::handle_retract_button(*s.h.panel);
        s.settle();
        CHECK(s.sent("G1 E-10 F1200"));
    }
}
