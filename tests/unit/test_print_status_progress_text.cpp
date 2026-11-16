// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_progress_text.cpp
 * @brief The progress card's text subjects follow the print's timings and speed.
 */

#include "ui_panel_print_status.h"

#include "../test_helpers/print_status_panel_fixture.h"
#include "print_progress_text.h"
#include "tune_controller.h"

#include <string>

#include "../catch_amalgamated.hpp"

using print_status_panel_test::PrintStatusPanelFixture;

namespace {

std::string subject_text(const char* name) {
    lv_subject_t* subject = lv_xml_get_subject(nullptr, name);
    REQUIRE(subject != nullptr);
    return lv_subject_get_string(subject);
}

} // namespace

TEST_CASE_METHOD(PrintStatusPanelFixture,
                 "Print status: elapsed and remaining text follow a running print",
                 "[print_status][progress_text]") {
    state().update_from_status({{"print_stats", {{"state", "printing"}, {"filename", "a.gcode"}}}});
    helix::ui::UpdateQueue::instance().drain();
    state().update_from_status({{"print_stats", {{"total_duration", 3720.0}}}});
    helix::ui::UpdateQueue::instance().drain();

    CHECK(subject_text("print_elapsed") == "1h 02m");
}

TEST_CASE_METHOD(PrintStatusPanelFixture,
                 "Print status: speed chip shows the live toolhead speed and volumetric flow",
                 "[print_status][progress_text]") {
    state().update_from_status({{"motion_report", {{"live_velocity", 187.4}}}});
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(50); // the chip update is coalesced onto a timer

    CHECK(subject_text("ps_chip_speed_flow_text") == "187 mm/s\n0.0 mm\xC2\xB3/s");

    // 5 mm/s of 1.75 mm filament; the extruder moving alone updates the chip.
    state().update_from_status({{"motion_report", {{"live_extruder_velocity", 5.0}}}});
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(50);

    CHECK(subject_text("ps_chip_speed_flow_text") == "187 mm/s\n12.0 mm\xC2\xB3/s");
}

TEST_CASE_METHOD(PrintStatusPanelFixture,
                 "Print status: the speed text before any reading uses the live rates format",
                 "[print_status][progress_text]") {
    PrintLifecycleState lifecycle;
    helix::ui::PrintProgressText text(state(), lifecycle);
    CHECK(std::string(text.speed_text()) == helix::tune::live_rates_text(0, 0.0));
}

TEST_CASE_METHOD(PrintStatusPanelFixture,
                 "Print status: a cancel that parks the head keeps the layer's height (#2055)",
                 "[print_status][progress_text][layer_line_z]") {
    state().update_from_status({{"print_stats", {{"state", "printing"}, {"filename", "a.gcode"}}}});
    state().print_state().set_print_layer_total(6);
    state().print_state().set_print_layer_heights(0.2, 0.3);
    helix::ui::UpdateQueue::instance().drain();
    state().update_from_status({{"print_stats", {{"print_duration", 30.0}}},
                                {"gcode_move", {{"gcode_position", {10.0, 10.0, 0.5, 0.0}}}}});
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(subject_text("print_layer_text") == "2 / 6 · 0.5mm");

    state().update_from_status({{"print_stats", {{"state", "cancelled"}}},
                                {"gcode_move", {{"gcode_position", {10.0, 10.0, 110.2, 0.0}}}}});
    helix::ui::UpdateQueue::instance().drain();
    CHECK(subject_text("print_layer_text") == "2 / 6 · 0.5mm");
}
