// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_temp_display_hide_target_when_off.cpp
 * @brief hide_target_when_off="true" shows the "/ target" portion only while a target is set.
 *
 * The home-screen heater tiles (panel_widget_temperature.xml and its bed /
 * chamber siblings) want the target visible during a heat-up and gone once the
 * heater is off — an idle tile reading "23.5 / —°C" spends a permanent third of
 * its width on a placeholder. Control surfaces keep show_target="true", where
 * the "—" is meaningful ("this heater is off"), so the two modes must stay
 * distinguishable: every assertion below is paired against a show_target="true"
 * widget bound to the SAME subjects, which is what proves hide_target_when_off
 * is doing the hiding rather than some unrelated visibility rule.
 */

#include "ui_temp_display.h"
#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "printer_state.h"
#include "printer_temperature_state.h"

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;

namespace {

// ui_temp_display_create_cb() creates its labels in a fixed order:
// current (0), separator (1), target (2), unit (3). Indices 1 and 2 exist only
// when show_target is enabled at all.
constexpr int SEPARATOR_CHILD = 1;
constexpr int TARGET_CHILD = 2;

lv_obj_t* create_display(lv_obj_t* parent, const char* show_target, const char* hide_when_off) {
    const char* attrs[] = {"bind_current",         "extruder_temp", "bind_target",
                           "extruder_target",      "show_target",   show_target,
                           "hide_target_when_off", hide_when_off,   nullptr};
    return static_cast<lv_obj_t*>(lv_xml_create(parent, "temp_display", attrs));
}

bool target_portion_hidden(lv_obj_t* display) {
    lv_obj_t* separator = lv_obj_get_child(display, SEPARATOR_CHILD);
    lv_obj_t* target = lv_obj_get_child(display, TARGET_CHILD);
    REQUIRE(separator != nullptr);
    REQUIRE(target != nullptr);
    bool sep_hidden = lv_obj_has_flag(separator, LV_OBJ_FLAG_HIDDEN);
    bool target_hidden = lv_obj_has_flag(target, LV_OBJ_FLAG_HIDDEN);
    // The two move together or the readout renders a dangling " / ".
    REQUIRE(sep_hidden == target_hidden);
    return sep_hidden;
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "hide_target_when_off hides the target while the heater is off",
                 "[temp_display][1267]") {
    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_t* hides_when_off = create_display(container, "true", "true");
    lv_obj_t* always = create_display(container, "true", "false");
    REQUIRE(hides_when_off != nullptr);
    REQUIRE(always != nullptr);

    lv_subject_t* target_subj = state().get_active_extruder_target_subject();
    lv_subject_t* current_subj = state().get_active_extruder_temp_subject();

    auto settle = [&](int current_deci, int target_deci) {
        lv_subject_set_int(target_subj, target_deci);
        lv_subject_set_int(current_subj, current_deci);
        UpdateQueue::instance().drain();
    };

    SECTION("heater off at creation — hidden before any subject update arrives") {
        // Deliberately asserts without settle(): a widget that only hid itself
        // on the first update would flash "/ —" for one frame on every rebuild.
        CHECK(target_portion_hidden(hides_when_off));
        CHECK_FALSE(target_portion_hidden(always));
    }

    SECTION("target set — both modes show it") {
        settle(1342, 2200);
        CHECK_FALSE(target_portion_hidden(hides_when_off));
        CHECK_FALSE(target_portion_hidden(always));
    }

    SECTION("target cleared after heating — hidden again") {
        settle(1342, 2200);
        REQUIRE_FALSE(target_portion_hidden(hides_when_off));

        settle(2100, 0);
        CHECK(target_portion_hidden(hides_when_off));
        CHECK_FALSE(target_portion_hidden(always));
    }

    SECTION("still hidden while off even at a nonzero current temp") {
        // A hot nozzle cooling down with no target is the common post-print
        // state; "210 / —°C" is exactly the readout the tiles are avoiding.
        settle(2100, 0);
        CHECK(target_portion_hidden(hides_when_off));
    }
}

TEST_CASE_METHOD(XMLTestFixture, "hide_target_when_off still builds the target labels",
                 "[temp_display][1267]") {
    // hide_target_when_off must hide, not skip creation — show_target="false" gives a
    // two-child widget with no separator/target at all, and a widget that took
    // that path could never reveal a target once one was set.
    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_t* hides_when_off = create_display(container, "true", "true");
    lv_obj_t* disabled = create_display(container, "false", "false");
    REQUIRE(hides_when_off != nullptr);
    REQUIRE(disabled != nullptr);

    CHECK(lv_obj_get_child_count(hides_when_off) == 4);
    CHECK(lv_obj_get_child_count(disabled) == 2);
}
