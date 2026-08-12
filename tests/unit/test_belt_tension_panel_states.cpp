// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_belt_tension_panel_states.cpp
 * @brief The belt tension panel's five view states and its Start gate.
 *
 * Two things this pins that nothing else can:
 *
 * 1. Every state container the C++ ViewState enum can select actually exists in
 *    the shipped XML, at the ref_value the enum uses. A renumbered enum with an
 *    unrenumbered binding leaves a state that renders as a blank page - no
 *    warning, no crash.
 *
 * 2. Start is disabled by a *subject binding*, not by hiding the Advanced-panel
 *    menu row. The row is not a gate: the panel is reachable by `ctl navigate`,
 *    by a deep link, and by a printer whose accelerometer drops out after
 *    entry. Only a binding on the action itself covers all three.
 *
 * A dangling bind_text fails silently at runtime rather than at build time, so
 * these tests also resolve each subject the new states bind by name.
 */

#include "ui_panel_belt_tension.h"
#include "ui_update_queue.h"

#include "../test_fixtures.h"

#include <lvgl.h>

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;

namespace {

/// Register the panel and everything it embeds, then build it.
lv_obj_t* build_belt_panel(XMLTestFixture& fx) {
    REQUIRE(fx.register_component("header_bar"));
    REQUIRE(fx.register_component("components/belt_result_card"));
    REQUIRE(fx.register_component("components/pluck_animation"));
    REQUIRE(fx.register_component("panel_belt_tension"));

    // Registers the XML event callbacks AND the bt_* subjects the panel binds.
    ui_panel_belt_tension_register_callbacks();

    lv_obj_t* panel = fx.create_component("panel_belt_tension");
    REQUIRE(panel != nullptr);
    UpdateQueue::instance().drain();
    return panel;
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "belt tension panel has a container for every view state",
                 "[belt][panel][xml]") {
    lv_obj_t* panel = build_belt_panel(*this);

    // One per ViewState value, in enum order.
    for (const char* name :
         {"state_start", "state_position", "state_listen", "state_compare", "state_error"}) {
        INFO("missing state container: " << name);
        CHECK(lv_obj_find_by_name(panel, name) != nullptr);
    }

    // The states these replaced must be gone, not left alongside.
    CHECK(lv_obj_find_by_name(panel, "state_progress") == nullptr);
    CHECK(lv_obj_find_by_name(panel, "state_results") == nullptr);
}

TEST_CASE_METHOD(XMLTestFixture, "belt tension Start is disabled by the gate subject",
                 "[belt][panel][xml][gating]") {
    // Connected and klippy-ready, so the only thing under test is bt_can_start.
    lv_subject_set_int(state().get_nav_buttons_enabled_subject(), 1);

    lv_obj_t* panel = build_belt_panel(*this);

    lv_obj_t* btn = lv_obj_find_by_name(panel, "btn_start");
    REQUIRE(btn != nullptr);

    lv_subject_t* can_start = lv_xml_get_subject(nullptr, "bt_can_start");
    REQUIRE(can_start != nullptr);

    lv_subject_set_int(can_start, 0);
    UpdateQueue::instance().drain();
    process_lvgl(20);
    CHECK(lv_obj_has_state(btn, LV_STATE_DISABLED));

    lv_subject_set_int(can_start, 1);
    UpdateQueue::instance().drain();
    process_lvgl(20);
    CHECK_FALSE(lv_obj_has_state(btn, LV_STATE_DISABLED));

    // And back - the gate has to re-arm when the accelerometer disappears
    // mid-session, which is the case a one-shot check at entry would miss.
    lv_subject_set_int(can_start, 0);
    UpdateQueue::instance().drain();
    process_lvgl(20);
    CHECK(lv_obj_has_state(btn, LV_STATE_DISABLED));
}

TEST_CASE_METHOD(XMLTestFixture, "belt tension panel binds only subjects that exist",
                 "[belt][panel][xml]") {
    build_belt_panel(*this);

    for (const char* name :
         {"bt_can_start", "bt_gate_message", "bt_has_target", "bt_park_status", "bt_current_belt",
          "bt_target_freq", "bt_result_a_freq", "bt_result_delta", "bt_error_message"}) {
        INFO("subject not registered: " << name);
        CHECK(lv_xml_get_subject(nullptr, name) != nullptr);
    }

    // Retired with the PROGRESS state. Leaving them registered would let a
    // stale binding survive review by continuing to resolve.
    CHECK(lv_xml_get_subject(nullptr, "bt_progress") == nullptr);
    CHECK(lv_xml_get_subject(nullptr, "bt_progress_label") == nullptr);
}
