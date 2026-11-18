// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_temp_display_trailing_target.cpp
 * @brief trailing_target="true" reads "205.3°C / 210°", and "/ Off" with the
 *        heater off: the form every heater row reads in.
 */

#include "ui_temp_display.h"

#include "../test_fixtures.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "printer_state.h"
#include "printer_temperature_state.h"

#include <string>

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;

namespace {

/// Every visible label of @p display, in order, joined. The gap after the separator
/// is its right margin, so the joined text reads "205°C /210°".
std::string shown_text(lv_obj_t* display) {
    std::string out;
    for (uint32_t i = 0; i < lv_obj_get_child_count(display); ++i) {
        lv_obj_t* label = lv_obj_get_child(display, static_cast<int32_t>(i));
        if (!lv_obj_is_hidden(label)) {
            out += lv_label_get_text(label);
        }
    }
    return out;
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "trailing_target reads the reading, then the target or Off",
                 "[temp_display][heater_row]") {
    lv_obj_t* container = lv_obj_create(test_screen());
    const char* attrs[] = {"bind_current",    "extruder_temp", "bind_target",
                           "extruder_target", "show_target",   "true",
                           "trailing_target", "true",          nullptr};
    auto* display = static_cast<lv_obj_t*>(lv_xml_create(container, "temp_display", attrs));
    REQUIRE(display != nullptr);

    lv_subject_t* target_subj = state().temperature_state().get_active_extruder_target_subject();
    lv_subject_t* current_subj = state().temperature_state().get_active_extruder_temp_subject();
    auto settle = [&](int current_deci, int target_deci) {
        lv_subject_set_int(target_subj, target_deci);
        lv_subject_set_int(current_subj, current_deci);
        UpdateQueue::instance().drain();
    };

    SECTION("heater off: the target reads Off, never a dash") {
        settle(277, 0);
        CHECK(shown_text(display) == "27.7°C /Off");
    }

    SECTION("target set: the target follows its subject in whole degrees") {
        settle(2053, 2100);
        CHECK(shown_text(display) == "205°C /210°");
        settle(853, 2100);
        CHECK(shown_text(display) == "85.3°C /210°");
    }

    SECTION("the target is smaller than the reading") {
        settle(2053, 2100);
        const lv_font_t* reading =
            lv_obj_get_style_text_font(lv_obj_get_child(display, 0), LV_PART_MAIN);
        const lv_font_t* target =
            lv_obj_get_style_text_font(lv_obj_get_child(display, 3), LV_PART_MAIN);
        CHECK(lv_font_get_line_height(target) < lv_font_get_line_height(reading));
    }
}
