// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_chamber.cpp
 * @brief The print status chamber chip against the real panel subjects: the
 *        effective setpoint (heater-OR-fan) and the control mode drive it, so
 *        an M141 set parked on the cooling fan - invisible in the raw heater
 *        target - still shows its target, and a set that lands without the
 *        temperature moving still recomputes.
 */

#include "ui_panel_print_status.h"
#include "ui_temperature_utils.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/print_status_panel_test_access.h"
#include "app_globals.h"
#include "printer_state.h"
#include "printer_temperature_state.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

// Same shape as FanPanelFixture (test_print_status_fan_section.cpp): the panel
// over the global PrinterState, with the deinit that unpublishes its subjects.
// The state's subjects are initialized BEFORE the panel is constructed: the
// panel's observers carry PrinterState's subjects-lifetime token, and a
// re-init after construction invalidates that token and silently kills them.
struct ChamberPanelFixture : public LVGLUITestFixture {
    ChamberPanelFixture() {
        get_printer_state().capabilities_state().init_subjects(false);
        auto& temps = get_printer_state().temperature_state();
        temps.init_subjects(false);
        temps.set_chamber_heater_name("heater_generic chamber_heater");
        temps.set_chamber_cooling_fan_name("temperature_fan chamber_fan");
        temps.set_chamber_fan_resting(350); // 35.0C in decidegrees (K2 stock)
        get_printer_state().capabilities_state().set_has_chamber_heater(true);
        panel = std::make_unique<PrintStatusPanel>(get_printer_state(), nullptr);
        panel->init_subjects();
    }
    ~ChamberPanelFixture() override {
        panel->deinit_subjects();
        panel.reset();
        // Return the global state to names and a capability later tests can
        // re-derive; a leaked HasChamberHeater would ghost the chamber row in
        // any test that builds XML afterwards.
        get_printer_state().capabilities_state().set_has_chamber_heater(false);
        auto& temps = get_printer_state().temperature_state();
        temps.set_chamber_heater_name("");
        temps.set_chamber_cooling_fan_name("");
        temps.set_chamber_fan_resting(0);
        temps.deinit_subjects();
        get_printer_state().capabilities_state().deinit_subjects();
    }

    /// Deliver the observer fires, let the chip update they scheduled run on
    /// its timer, and read the chamber chip's text ("" while hidden).
    std::string chip() {
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(50);
        lv_subject_t* shown = lv_xml_get_subject(nullptr, "ps_chip_chamber_shown");
        lv_subject_t* text = lv_xml_get_subject(nullptr, "ps_chip_chamber_text");
        REQUIRE(shown != nullptr);
        REQUIRE(text != nullptr);
        return lv_subject_get_int(shown) ? lv_subject_get_string(text) : "";
    }

    std::unique_ptr<PrintStatusPanel> panel;
};

} // namespace

TEST_CASE_METHOD(ChamberPanelFixture, "print status chamber chip follows the effective setpoint",
                 "[print_status][chamber][m141]") {
    auto& temps = get_printer_state().temperature_state();

    // Resting: heater 0, fan parked at its configured 35 - the reading alone.
    temps.update_from_status(
        {{"heater_generic chamber_heater", {{"temperature", 30.3}, {"target", 0.0}}},
         {"temperature_fan chamber_fan", {{"target", 35.0}}}});
    REQUIRE(lv_subject_get_int(temps.get_chamber_effective_target_subject()) == 350);
    CHECK(chip() == "30°");

    // Deliberate 30 differs from resting: Maintaining, holding.
    temps.update_from_status({{"heater_generic chamber_heater", {{"temperature", 30.3}}},
                              {"temperature_fan chamber_fan", {{"target", 30.0}}}});
    REQUIRE(lv_subject_get_int(temps.get_chamber_mode_subject()) == ChamberMode::Maintaining);
    CHECK(chip() == "30°");

    // Above the ceiling: cooling toward the target, which shows again.
    temps.update_from_status({{"heater_generic chamber_heater", {{"temperature", 36.1}}},
                              {"temperature_fan chamber_fan", {{"target", 30.0}}}});
    CHECK(chip() == "36 / 30°");

    // Nothing holds a target and the chamber is cool: the reading still shows,
    // since the chamber's temperature matters to the print either way.
    temps.update_from_status(
        {{"heater_generic chamber_heater", {{"temperature", 30.3}, {"target", 0.0}}},
         {"temperature_fan chamber_fan", {{"target", 0.0}}}});
    REQUIRE(lv_subject_get_int(temps.get_chamber_effective_target_subject()) == 0);
    CHECK(chip() == "30°");
}

TEST_CASE_METHOD(ChamberPanelFixture,
                 "print status chamber chip recomputes when only the target moves",
                 "[print_status][chamber][m141]") {
    auto& temps = get_printer_state().temperature_state();

    temps.update_from_status(
        {{"heater_generic chamber_heater", {{"temperature", 36.1}, {"target", 0.0}}},
         {"temperature_fan chamber_fan", {{"target", 30.0}}}});
    REQUIRE(lv_subject_get_int(temps.get_chamber_mode_subject()) == ChamberMode::Maintaining);
    REQUIRE(chip() == "36 / 30°");

    // Same temperature and mode, only the setpoint moves (M141 S32 landing
    // mid-print): neither the chamber temp nor the mode subject fires, so this
    // recomputes only through the setpoint observer.
    temps.update_from_status({{"temperature_fan chamber_fan", {{"target", 32.0}}}});
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(lv_subject_get_int(temps.get_chamber_temp_subject()) == 361);
    REQUIRE(lv_subject_get_int(temps.get_chamber_mode_subject()) == ChamberMode::Maintaining);
    CHECK(chip() == "36 / 32°");
}
