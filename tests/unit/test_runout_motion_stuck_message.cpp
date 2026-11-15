// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_runout_motion_stuck_message.cpp
 * @brief On an autofeed backend the runout dialog tells a real runout (the port
 *        sees no filament) from a motion-sensor trip with filament still at the
 *        port.
 */

#include "ui_runout_guidance_modal.h"

#include "../lvgl_ui_test_fixture.h"
#include "helix-xml/src/xml/lv_xml.h"

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
