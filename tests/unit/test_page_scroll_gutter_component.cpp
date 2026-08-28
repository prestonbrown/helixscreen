// SPDX-License-Identifier: GPL-3.0-or-later
// TEST_MIRROR_OK: drives a ui_xml component through the XML fixture at runtime
#include "../lvgl_ui_test_fixture.h"

#include "../catch_amalgamated.hpp"

TEST_CASE_METHOD(LVGLUITestFixture, "page_scroll_gutter component creates named up/down buttons",
                 "[page_scroll_buttons][ui]") {
    auto* gutter =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "page_scroll_gutter", nullptr));
    REQUIRE(gutter != nullptr);
    CHECK(lv_obj_find_by_name(gutter, "up") != nullptr);
    CHECK(lv_obj_find_by_name(gutter, "down") != nullptr);
}
