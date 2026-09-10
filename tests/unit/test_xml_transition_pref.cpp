// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_animations_pref.h"

#include "../lvgl_test_fixture.h"
#include "helix-xml/src/xml/lv_xml_style.h"

#include "../catch_amalgamated.hpp"

TEST_CASE_METHOD(LVGLTestFixture, "animations pref drives the XML transition scale",
                 "[xml][transition]") {
    lv_subject_t* pref = helix::ui::animations_pref_subject(nullptr);
    REQUIRE(pref != nullptr);

    /* lv_subject_set_int notifies observers synchronously on the calling
     * thread, so no timer pump is needed here. */
    lv_subject_set_int(pref, 1);
    REQUIRE(lv_xml_get_transition_scale() == 256);

    lv_subject_set_int(pref, 0);
    REQUIRE(lv_xml_get_transition_scale() == 0);
}
