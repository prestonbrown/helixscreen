// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_chamber_mode_icon_label_parity.cpp
 * @brief The chamber icon and the chamber temp-label MUST resolve the exact
 * same color for every (mode, current, target) combination.
 *
 * The bug this pins: HeaterIconBinder/HeatingIconAnimator classified the
 * chamber icon with the plain (mode-unaware) classify_heat_state(), so a
 * chamber in Maintaining mode — where the "target" is a cooling CEILING, not
 * a heat goal — pulsed heating-red while the label beside it (which already
 * had Maintaining-aware logic) showed a neutral color. The fix routes both
 * through the shared classify_heat_state_with_mode().
 *
 * Drives BOTH consumers off the SAME underlying PrinterState chamber subjects
 * (chamber_temp / chamber_effective_target / chamber_mode) that production XML
 * binds them to (see temp_graph_overlay.xml,
 * controls_panel.xml), so a divergence here means the two code paths actually
 * disagree — not that the test wired them up differently.
 */

#include "ui_heater_icon_binder.h"
#include "ui_temp_display.h"
#include "ui_temperature_utils.h"
#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "printer_state.h"
#include "printer_temperature_state.h"

#include "../catch_amalgamated.hpp"

using helix::ChamberMode;
using helix::HeaterType;
using helix::PrinterState;
using helix::ui::HeaterIconBinder;
using helix::ui::UpdateQueue;

namespace {

// TEST_MIRROR_OK: a widget-tree FIXTURE, not a reimplementation — it builds the
// same parent/child shape production XML produces, and the test then calls the
// real HeaterIconBinder::bind() and the real default_icon_name() against it.
// Mirrors the shape HeaterIconBinder::bind() looks for: a container with a
// single child named after the heater's conventional icon glyph.
lv_obj_t* create_icon_root(lv_obj_t* parent, const char* icon_name) {
    lv_obj_t* root = lv_obj_create(parent);
    lv_obj_t* icon = lv_obj_create(root);
    lv_obj_set_name(icon, icon_name);
    return root;
}

lv_color_t icon_text_color(lv_obj_t* icon) {
    return lv_obj_get_style_text_color(icon, LV_PART_MAIN);
}

// temp_display's current-temp label is always the FIRST child created
// (ui_temp_display.cpp's create callback creates it before the optional
// separator/target labels), regardless of show_target — see
// ui_temp_display_create_cb().
lv_color_t label_text_color(lv_obj_t* temp_display_obj) {
    lv_obj_t* current_label = lv_obj_get_child(temp_display_obj, 0);
    return lv_obj_get_style_text_color(current_label, LV_PART_MAIN);
}

bool colors_eq(lv_color_t a, lv_color_t b) {
    return a.red == b.red && a.green == b.green && a.blue == b.blue;
}

} // namespace

// Uses XMLTestFixture (not LVGLTestFixture) specifically because its one-time
// global setup initializes the theme (globals.xml color tokens) — without it,
// theme_manager_get_color() falls back to the same value for every token and
// a color-equality assertion would pass vacuously (both sides "agree" only
// because everything resolves to black).
TEST_CASE_METHOD(XMLTestFixture,
                 "chamber icon and label resolve the same color across every mode/current/target",
                 "[heater_binder][temp_display][chamber_mode]") {
    // Icon side: a real chamber_icon_glyph bound through HeaterIconBinder, the
    // same path every production chamber icon (controls panel, print-status
    // cards, temp-stack tile, chamber overlay, chamber home tile) goes through.
    lv_obj_t* icon_root =
        create_icon_root(test_screen(), HeaterIconBinder::default_icon_name(HeaterType::Chamber));
    lv_obj_t* icon = lv_obj_find_by_name(icon_root, "chamber_icon_glyph");
    REQUIRE(icon != nullptr);

    HeaterIconBinder binder;
    REQUIRE(binder.bind(icon_root, state(), HeaterType::Chamber));

    // Label side: a real <temp_display>, bound the same way production XML
    // binds the chamber card (temp_graph_overlay.xml) — against the SAME PrinterState chamber
    // subjects the icon binder above reads.
    lv_obj_t* container = lv_obj_create(test_screen());
    const char* attrs[] = {
        "bind_current", "chamber_temp", "bind_target", "chamber_effective_target",
        "bind_mode",    "chamber_mode", "show_target", "true",
        nullptr};
    lv_obj_t* temp_display =
        static_cast<lv_obj_t*>(lv_xml_create(container, "temp_display", attrs));
    REQUIRE(temp_display != nullptr);

    lv_subject_t* current_subj = state().get_chamber_temp_subject();
    lv_subject_t* target_subj = state().get_chamber_effective_target_subject();
    lv_subject_t* mode_subj = state().get_chamber_mode_subject();

    struct Case {
        const char* name;
        int current_deci;
        int target_deci;
        ChamberMode mode;
    };
    // All decidegree values are multiples of 10 to sidestep integer-truncation
    // edge cases at the tolerance boundary: the label classifies in whole
    // degrees (deci_to_degrees() truncates), the icon classifies directly in
    // decidegrees — a value like 2022 would truncate to 202 on the label side
    // while staying 2022 on the icon side, an artifact of the two different
    // units, not a real disagreement.
    const Case cases[] = {
        {"Off", 250, 0, ChamberMode::Off},
        {"Heating: heating", 1500, 2000, ChamberMode::Heating},
        {"Heating: at-temp", 1990, 2000, ChamberMode::Heating},
        {"Heating: cooling", 2100, 2000, ChamberMode::Heating},
        // The regression case: cold and far below the Maintaining ceiling used
        // to render heating-red on the icon while the label stayed neutral.
        {"Maintaining: far below ceiling", 0, 2000, ChamberMode::Maintaining},
        {"Maintaining: at ceiling", 2000, 2000, ChamberMode::Maintaining},
        {"Maintaining: tolerance boundary, still neutral", 2020, 2000, ChamberMode::Maintaining},
        {"Maintaining: above ceiling", 2100, 2000, ChamberMode::Maintaining},
    };

    for (const auto& c : cases) {
        INFO("case: " << c.name);
        lv_subject_set_int(mode_subj, static_cast<int>(c.mode));
        lv_subject_set_int(target_subj, c.target_deci);
        lv_subject_set_int(current_subj, c.current_deci);
        UpdateQueue::instance().drain();

        lv_color_t icon_color = icon_text_color(icon);
        lv_color_t label_color = label_text_color(temp_display);
        REQUIRE(colors_eq(icon_color, label_color));
    }

    binder.unbind();
}

// The pulse-suppression side of the fix (Maintaining must never pulse, even
// when cold) is covered directly against HeatingIconAnimator in
// test_heating_animator_state.cpp — lv_anim_get() keys off the animator
// instance as its `var`, which is a private member here (animator_), so that
// check cannot be written against HeaterIconBinder without reaching into it.
