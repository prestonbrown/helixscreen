// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The calibration intro pieces every step's intro is built from: the footer's
// primary gate and the choice chip's tap index.

#include "../test_fixtures.h"
#include "calibration_intro_text.h"
#include "core/update_queue.h"
#include "printer_state.h"

#include <lvgl.h>

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;

namespace {

int g_last_chip_index = -2;

void record_chip(lv_event_t* e) {
    g_last_chip_index = helix::calibration::intro_chip_index(e);
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture,
                 "the intro primary is disabled while the printer is unreachable or held",
                 "[calibration_intro][xml]") {
    REQUIRE(register_component("components/calibration_intro_footer"));
    lv_obj_t* footer = create_component("calibration_intro_footer");
    REQUIRE(footer != nullptr);
    lv_obj_t* primary = lv_obj_find_by_name(footer, "cal_intro_primary");
    REQUIRE(primary != nullptr);

    lv_subject_t* nav = state().network_state().get_nav_buttons_enabled_subject();
    lv_subject_set_int(nav, 1);
    UpdateQueue::instance().drain_until_empty();
    CHECK_FALSE(lv_obj_has_state(primary, LV_STATE_DISABLED));

    SECTION("an unreachable printer") {
        lv_subject_set_int(nav, 0);
        UpdateQueue::instance().drain_until_empty();
        CHECK(lv_obj_has_state(primary, LV_STATE_DISABLED));
        lv_subject_set_int(nav, 1);
        UpdateQueue::instance().drain_until_empty();
        CHECK_FALSE(lv_obj_has_state(primary, LV_STATE_DISABLED));
    }

    SECTION("a job holding the machine") {
        state().update_from_status(nlohmann::json{{"print_stats", {{"state", "standby"}}}});
        state().print_state().set_print_start_state(helix::PrintStartPhase::BED_MESH, "", 0);
        UpdateQueue::instance().drain_until_empty();
        CHECK(lv_obj_has_state(primary, LV_STATE_DISABLED));
        state().print_state().set_print_start_state(helix::PrintStartPhase::IDLE, "", 0);
        UpdateQueue::instance().drain_until_empty();
        CHECK_FALSE(lv_obj_has_state(primary, LV_STATE_DISABLED));
    }
}

TEST_CASE_METHOD(XMLTestFixture, "a choice chip reports its index and shows its choice",
                 "[calibration_intro][xml]") {
    lv_xml_register_event_cb(nullptr, "test_cal_intro_chip_cb", record_chip);
    static helix::Subject choice;
    choice.init_int(0);
    lv_xml_register_subject(nullptr, "test_cal_intro_choice", &choice);

    REQUIRE(register_component("components/cal_intro_chip"));
    const char* attrs[] = {"label", "PETG", "choice",   "test_cal_intro_choice",  "value", "2",
                           "index", "2",    "callback", "test_cal_intro_chip_cb", nullptr};
    lv_obj_t* chip = create_component("cal_intro_chip", attrs);
    REQUIRE(chip != nullptr);

    g_last_chip_index = -2;
    lv_obj_send_event(chip, LV_EVENT_CLICKED, nullptr);
    CHECK(g_last_chip_index == 2);

    CHECK_FALSE(lv_obj_has_state(chip, LV_STATE_CHECKED));
    lv_subject_set_int(&choice, 2);
    CHECK(lv_obj_has_state(chip, LV_STATE_CHECKED));

    lv_obj_delete(chip);
    choice.reset();
}
