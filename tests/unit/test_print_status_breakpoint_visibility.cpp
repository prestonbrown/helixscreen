// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_breakpoint_visibility.cpp
 * @brief print_status_panel's speed/flow row hides on the two narrow breakpoints
 *
 * The row is suppressed at TINY and SMALL, where the panel has no width to
 * spare, and shown everywhere else. ui_breakpoint publishes the tier as an int:
 * 0=MICRO, 1=TINY, 2=SMALL, 3=MEDIUM, 4=LARGE, 5=XLARGE, 6=XXLARGE.
 *
 * Expressing that as two bindings on one flag cannot work, and unlike the usual
 * clobber it does not even depend on notification order. Both observers watch
 * the SAME subject, so every publish fires both, and the second always resolves
 * last: at TINY the first says hide and the second says show, so the row is
 * visible at exactly the size the markup means to hide it. One expression
 * binding naming both tiers is what actually suppresses the row.
 */

#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "../test_helpers/xml_bind_test_utils.h"

#include <lvgl.h>

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;
using helix::test::require_named;
using helix::test::set_xml_subject;

namespace {

// Named rather than inlined: a bare 1 or 2 in an assertion says nothing about
// which screen tier is being claimed.
constexpr int kMicro = 0;
constexpr int kTiny = 1;
constexpr int kSmall = 2;
constexpr int kMedium = 3;
constexpr int kXXLarge = 6;

bool row_hidden(lv_obj_t* panel) {
    return lv_obj_has_flag(require_named(panel, "speed_flow_row"), LV_OBJ_FLAG_HIDDEN);
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "speed/flow row is hidden at the two narrow breakpoints",
                 "[ui][print_status_panel][bind_flag][breakpoint_visibility]") {
    REQUIRE(register_component("print_status_panel"));

    set_xml_subject("ui_breakpoint", kMedium);
    lv_obj_t* panel = create_component("print_status_panel");
    REQUIRE(panel != nullptr);

    SECTION("TINY hides it") {
        set_xml_subject("ui_breakpoint", kTiny);
        UpdateQueue::instance().drain();
        CHECK(row_hidden(panel));
    }

    SECTION("SMALL hides it") {
        set_xml_subject("ui_breakpoint", kSmall);
        UpdateQueue::instance().drain();
        CHECK(row_hidden(panel));
    }
}

TEST_CASE_METHOD(XMLTestFixture, "speed/flow row is shown at every other breakpoint",
                 "[ui][print_status_panel][bind_flag][breakpoint_visibility]") {
    REQUIRE(register_component("print_status_panel"));

    set_xml_subject("ui_breakpoint", kMedium);
    lv_obj_t* panel = create_component("print_status_panel");
    REQUIRE(panel != nullptr);

    // MICRO is deliberately outside the hidden set, so it is the case that
    // separates "hides on the two narrow tiers" from "hides on anything small".
    for (int bp : {kMicro, kMedium, kXXLarge}) {
        INFO("ui_breakpoint: " << bp);
        set_xml_subject("ui_breakpoint", bp);
        UpdateQueue::instance().drain();
        CHECK_FALSE(row_hidden(panel));
    }
}

TEST_CASE_METHOD(XMLTestFixture, "speed/flow row follows the breakpoint back and forth",
                 "[ui][print_status_panel][bind_flag][breakpoint_visibility]") {
    REQUIRE(register_component("print_status_panel"));

    set_xml_subject("ui_breakpoint", kMedium);
    lv_obj_t* panel = create_component("print_status_panel");
    REQUIRE(panel != nullptr);

    // Without a round trip the binding could be a one-shot applied at build
    // time, and the assertions above would hold for the wrong reason.
    CHECK_FALSE(row_hidden(panel));

    set_xml_subject("ui_breakpoint", kTiny);
    UpdateQueue::instance().drain();
    CHECK(row_hidden(panel));

    set_xml_subject("ui_breakpoint", kMedium);
    UpdateQueue::instance().drain();
    CHECK_FALSE(row_hidden(panel));
}
