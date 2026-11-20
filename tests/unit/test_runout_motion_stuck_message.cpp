// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_runout_motion_stuck_message.cpp
 * @brief The runout dialog's conditional rows: on an autofeed backend it tells a
 *        real runout (the port sees no filament) from a motion-sensor trip with
 *        filament still at the port, and while paused it can offer another head.
 */

#include "ui_runout_guidance_modal.h"

#include "../lvgl_ui_test_fixture.h"
#include "core/static_subject_registry.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "printer_state.h"

#include <lvgl/lvgl.h>

#include "../catch_amalgamated.hpp"

namespace {

bool hidden(lv_obj_t* root, const char* name) {
    lv_obj_t* obj = lv_obj_find_by_name(root, name);
    REQUIRE(obj != nullptr);
    return lv_obj_is_hidden(obj);
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "Runout dialog: motion sensor stuck versus a real runout",
                 "[runout][autofeed]") {
    RunoutGuidanceModal modal;
    modal.set_autofeed_capable(true);
    modal.set_resume_blocked(true);

    auto* dialog =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "runout_guidance_modal", nullptr));
    REQUIRE(dialog != nullptr);

    // Port empty: a real runout.
    CHECK_FALSE(hidden(dialog, "dialog_message_autofeed"));
    CHECK(hidden(dialog, "dialog_message_motion_stuck"));
    CHECK(hidden(dialog, "dialog_message"));

    // Port still sees filament: only the motion sensor tripped.
    modal.set_resume_blocked(false);
    CHECK(hidden(dialog, "dialog_message_autofeed"));
    CHECK_FALSE(hidden(dialog, "dialog_message_motion_stuck"));

    SECTION("a basic sensor shows neither autofeed message") {
        modal.set_autofeed_capable(false);
        CHECK(hidden(dialog, "dialog_message_autofeed"));
        CHECK(hidden(dialog, "dialog_message_motion_stuck"));
        CHECK_FALSE(hidden(dialog, "dialog_message"));
    }

    modal.set_autofeed_capable(false);
    modal.set_resume_blocked(false);
    lv_obj_delete(dialog);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Runout dialog: Use Another Toolhead shows only when offered while paused",
                 "[runout][runout_other_head]") {
    lv_subject_t* print_state = state().print_state().get_print_state_enum_subject();
    lv_subject_set_int(print_state, static_cast<int>(helix::PrintJobState::PAUSED));
    RunoutGuidanceModal modal;
    int chosen = 0;
    modal.set_on_use_other_head([&chosen]() { ++chosen; });
    modal.set_other_head_offered(true);
    REQUIRE(modal.show(test_screen()));
    lv_obj_t* dialog = modal.dialog();
    REQUIRE(dialog != nullptr);

    CHECK_FALSE(hidden(dialog, "other_head_row"));
    lv_obj_send_event(lv_obj_find_by_name(dialog, "btn_other_head"), LV_EVENT_CLICKED, nullptr);
    CHECK(chosen == 1);
    CHECK(modal.is_visible()); // the chooser stacks on the dialog

    modal.set_other_head_offered(false);
    CHECK(hidden(dialog, "other_head_row"));
    modal.set_other_head_offered(true);
    lv_subject_set_int(print_state, static_cast<int>(helix::PrintJobState::PRINTING));
    CHECK(hidden(dialog, "other_head_row"));

    modal.hide();
    modal.set_other_head_offered(false);
    lv_subject_set_int(print_state, static_cast<int>(helix::PrintJobState::STANDBY));
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Runout dialog: its subjects leave the XML registry on deinit and come back "
                 "clean",
                 "[runout][runout_other_head]") {
    { RunoutGuidanceModal first; }
    REQUIRE(lv_xml_get_subject(nullptr, "runout_other_head_offered") != nullptr);
    REQUIRE(StaticSubjectRegistry::instance().deinit_one("RunoutGuidanceModal"));
    for (const char* name : {"runout_autofeed_capable", "runout_resume_blocked",
                             "runout_is_advisory", "runout_other_head_offered"}) {
        INFO(name);
        CHECK(lv_xml_get_subject(nullptr, name) == nullptr);
    }
    // A re-init over a subject the deinit left live is a strict anomaly.
    RunoutGuidanceModal second;
    CHECK(lv_xml_get_subject(nullptr, "runout_other_head_offered") != nullptr);
}
