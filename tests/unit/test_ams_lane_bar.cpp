// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_lane_bar.h"

#include "../test_fixtures.h"
#include "../ui_test_utils.h"
#include "ams_backend_mock.h"
#include "ams_lane_state.h"
#include "ams_state.h"
#include "helix-xml/src/xml/lv_xml.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {
lv_obj_t* make_bar(lv_obj_t* parent, int slot_index) {
    const std::string idx = std::to_string(slot_index);
    const char* attrs[] = {"slot_index", idx.c_str(), nullptr};
    return static_cast<lv_obj_t*>(lv_xml_create(parent, "ams_lane_bar", attrs));
}
bool visible(lv_obj_t* root, const char* name) {
    lv_obj_t* o = lv_obj_find_by_name(root, name);
    REQUIRE(o != nullptr);
    return !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN);
}
} // namespace

TEST_CASE_METHOD(XMLTestFixture, "ams_lane_bar: an Empty lane draws nothing", "[ams][lane_bar]") {
    // The whole point of the bar rendering's Empty case: no fill, no outline.
    // The layout gap remains so lanes stay countable, but nothing is painted.
    ui_ams_lane_bar_register();
    AmsState::instance().init_subjects(true);
    lv_subject_set_int(AmsState::instance().get_slot_lane_state_subject(0),
                       static_cast<int>(helix::ui::LaneState::Empty));

    lv_obj_t* bar = make_bar(test_screen(), 0);
    REQUIRE(bar != nullptr);
    process_lvgl(20);

    CHECK_FALSE(visible(bar, "bar_fill"));
    CHECK_FALSE(visible(bar, "bar_bg"));
    lv_obj_delete(bar);
}

TEST_CASE_METHOD(XMLTestFixture, "ams_lane_bar: a Ghosted lane keeps its fill, dimmed",
                 "[ams][lane_bar]") {
    // The #1071 reversal. BOTH halves are asserted: the fill comes back AND the
    // cell is dimmed. Without the second, this is the bug a106413f6 fixed.
    ui_ams_lane_bar_register();
    AmsState::instance().init_subjects(true);
    lv_subject_set_int(AmsState::instance().get_slot_lane_state_subject(0),
                       static_cast<int>(helix::ui::LaneState::Ghosted));
    lv_subject_set_int(AmsState::instance().get_slot_fill_subject(0), 60);

    lv_obj_t* bar = make_bar(test_screen(), 0);
    REQUIRE(bar != nullptr);
    process_lvgl(20);

    CHECK(visible(bar, "bar_fill"));
    CHECK(lv_obj_get_style_opa(bar, LV_PART_MAIN) < LV_OPA_COVER);
    lv_obj_delete(bar);
}

TEST_CASE_METHOD(XMLTestFixture, "ams_lane_bar: a Present lane is full strength",
                 "[ams][lane_bar]") {
    ui_ams_lane_bar_register();
    AmsState::instance().init_subjects(true);
    lv_subject_set_int(AmsState::instance().get_slot_lane_state_subject(0),
                       static_cast<int>(helix::ui::LaneState::Present));
    lv_subject_set_int(AmsState::instance().get_slot_fill_subject(0), 60);

    lv_obj_t* bar = make_bar(test_screen(), 0);
    REQUIRE(bar != nullptr);
    process_lvgl(20);

    CHECK(visible(bar, "bar_fill"));
    CHECK(lv_obj_get_style_opa(bar, LV_PART_MAIN) == LV_OPA_COVER);
    lv_obj_delete(bar);
}

TEST_CASE_METHOD(XMLTestFixture, "ams_lane_bar: decorations do not alter the base state",
                 "[ams][lane_bar]") {
    // Active and error are laid OVER a base state, not alternatives to it.
    // A blocked lane still has filament; an active lane is still Present.
    ui_ams_lane_bar_register();
    AmsState::instance().init_subjects(true);
    lv_subject_set_int(AmsState::instance().get_slot_lane_state_subject(0),
                       static_cast<int>(helix::ui::LaneState::Present));
    lv_subject_set_int(AmsState::instance().get_slot_fill_subject(0), 60);

    lv_obj_t* bar = make_bar(test_screen(), 0);
    REQUIRE(bar != nullptr);
    process_lvgl(20);
    REQUIRE(visible(bar, "bar_fill"));

    // Going active must not hide or dim the fill.
    lv_subject_set_int(AmsState::instance().get_slot_active_loaded_subject(0), 1);
    process_lvgl(20);
    CHECK(visible(bar, "bar_fill"));
    CHECK(lv_obj_get_style_opa(bar, LV_PART_MAIN) == LV_OPA_COVER);
    // ...and it does add its own mark.
    CHECK(lv_obj_get_style_border_width(lv_obj_find_by_name(bar, "bar_bg"), LV_PART_MAIN) == 2);

    lv_obj_delete(bar);
}

TEST_CASE_METHOD(XMLTestFixture, "ams_lane_bar: the lane_state subject drives repaint",
                 "[ams][lane_bar]") {
    // Proves the observer is wired, not just the initial apply.
    ui_ams_lane_bar_register();
    AmsState::instance().init_subjects(true);
    lv_subject_t* st = AmsState::instance().get_slot_lane_state_subject(0);
    lv_subject_set_int(st, static_cast<int>(helix::ui::LaneState::Present));
    lv_subject_set_int(AmsState::instance().get_slot_fill_subject(0), 60);

    lv_obj_t* bar = make_bar(test_screen(), 0);
    REQUIRE(bar != nullptr);
    process_lvgl(20);
    REQUIRE(visible(bar, "bar_fill"));

    lv_subject_set_int(st, static_cast<int>(helix::ui::LaneState::Empty));
    process_lvgl(20);
    CHECK_FALSE(visible(bar, "bar_fill"));
    lv_obj_delete(bar);
}
