// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_temp_display_target_only.cpp
 * @brief target_only="true" renders the target alone, for a target chip.
 *
 * The Controls temperature card shows the reading and the target as two
 * things: the reading beside the heater icon, the target in a tappable chip.
 * The chip's temp_display must draw the target and nothing else, while the
 * target still follows its subject and reads "—" with the heater off.
 */

#include "ui_temp_display.h"

#include "../test_fixtures.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "printer_state.h"
#include "printer_temperature_state.h"

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;

namespace {

// ui_temp_display_create_cb() creates its labels in a fixed order:
// current (0), separator (1), target (2), unit (3).
constexpr int kCurrentChild = 0;
constexpr int kSeparatorChild = 1;
constexpr int kTargetChild = 2;
constexpr int kUnitChild = 3;

bool hidden(lv_obj_t* display, int child) {
    lv_obj_t* label = lv_obj_get_child(display, child);
    REQUIRE(label != nullptr);
    return lv_obj_is_hidden(label);
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "target_only shows the target and nothing else",
                 "[temp_display][controls]") {
    lv_obj_t* container = lv_obj_create(test_screen());
    const char* attrs[] = {"bind_current",    "extruder_temp", "bind_target",
                           "extruder_target", "show_target",   "true",
                           "target_only",     "true",          nullptr};
    auto* chip = static_cast<lv_obj_t*>(lv_xml_create(container, "temp_display", attrs));
    REQUIRE(chip != nullptr);

    lv_subject_t* target_subj = state().temperature_state().get_active_extruder_target_subject();
    lv_subject_t* current_subj = state().temperature_state().get_active_extruder_temp_subject();
    auto settle = [&](int current_deci, int target_deci) {
        lv_subject_set_int(target_subj, target_deci);
        lv_subject_set_int(current_subj, current_deci);
        UpdateQueue::instance().drain();
    };

    auto check_only_target_drawn = [&]() {
        CHECK(hidden(chip, kCurrentChild));
        CHECK(hidden(chip, kSeparatorChild));
        CHECK_FALSE(hidden(chip, kTargetChild));
        CHECK(hidden(chip, kUnitChild));
    };

    SECTION("heater off: the chip reads the off placeholder") {
        settle(277, 0);
        check_only_target_drawn();
        CHECK(std::string(lv_label_get_text(lv_obj_get_child(chip, kTargetChild))) == "—");
    }

    SECTION("target set: the chip reads the target, and a later update keeps the rest hidden") {
        settle(277, 2100);
        settle(1342, 2100);
        check_only_target_drawn();
        CHECK(std::string(lv_label_get_text(lv_obj_get_child(chip, kTargetChild))) == "210");
    }
}
