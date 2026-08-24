// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_controls_secondary_temp_scale.cpp
 * @brief Regression test for the ControlsPanel secondary-temperature scale bug.
 *
 * TemperatureSensorManager publishes per-sensor temperatures as DECIdegrees
 * (degrees x 10 — see helix::units::to_decidegrees), so 45.0 C arrives on the
 * subject as 450. ControlsPanel's overflow temperature list divided the subject
 * value by *100*, rendering every secondary sensor at one tenth of its real
 * value: 45 C showed as "4C", 100 C as "10C".
 *
 * These tests assert the rendered LABEL STRING (not the arithmetic), so they
 * also pin the display format: whole degrees plus the degree sign, matching
 * helix::ui::temperature::format_temperature().
 */

#include "ui_panel_controls.h"
#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "../test_helpers/controls_panel_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "printer_state.h"

#include <string>

#include "../catch_amalgamated.hpp"

using helix::PrinterState;
using helix::ui::ControlsPanelTestAccess;
using helix::ui::UpdateQueueTestAccess;

namespace {

/// Render one decidegree value through the real observer-side update path and
/// return the resulting label text.
std::string render(ControlsPanel& panel, lv_obj_t* label, int decidegrees) {
    ControlsPanelTestAccess::set_temp_rows(panel, {{"temperature_sensor mcu", label}});
    ControlsPanelTestAccess::update_temp(panel, "temperature_sensor mcu", decidegrees);
    const char* txt = lv_label_get_text(label);
    return txt ? std::string(txt) : std::string();
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "ControlsPanel renders secondary temps at true scale",
                 "[controls][temps][units]") {
    PrinterState& st = state();
    ControlsPanel panel(st, nullptr);

    lv_obj_t* label = lv_label_create(lv_screen_active());
    REQUIRE(label != nullptr);

    // 450 decidegrees == 45.0 C. The pre-fix `/ 100` rendered "4°C".
    REQUIRE(render(panel, label, 450) == "45°C");

    // 1000 decidegrees == 100.0 C. The pre-fix `/ 100` rendered "10°C" — the
    // failure mode is worst exactly where it matters (hot MCU / hot chamber).
    REQUIRE(render(panel, label, 1000) == "100°C");

    // Room temperature: 21.5 C truncates to whole degrees for this compact row.
    REQUIRE(render(panel, label, 215) == "21°C");

    // Cold/zero and sub-zero sensors must not wrap or render as empty.
    REQUIRE(render(panel, label, 0) == "0°C");
    REQUIRE(render(panel, label, -55) == "-5°C");

    ControlsPanelTestAccess::set_temp_rows(panel, {});
    lv_obj_delete(label);
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
}

TEST_CASE_METHOD(XMLTestFixture, "ControlsPanel secondary temp update ignores unknown sensors",
                 "[controls][temps][units]") {
    PrinterState& st = state();
    ControlsPanel panel(st, nullptr);

    lv_obj_t* label = lv_label_create(lv_screen_active());
    REQUIRE(label != nullptr);
    lv_label_set_text(label, "untouched");

    ControlsPanelTestAccess::set_temp_rows(panel, {{"temperature_sensor mcu", label}});
    ControlsPanelTestAccess::update_temp(panel, "temperature_sensor chamber", 450);

    REQUIRE(std::string(lv_label_get_text(label)) == "untouched");

    ControlsPanelTestAccess::set_temp_rows(panel, {});
    lv_obj_delete(label);
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
}
