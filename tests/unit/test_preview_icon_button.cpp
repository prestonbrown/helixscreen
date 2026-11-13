// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_preview_icon_button.cpp
 * @brief The translucent icon button the preview corner controls share
 *
 * The print status card's camera and view-mode toggles and the skip-objects
 * button are all this one component, so their look cannot drift apart. The
 * host supplies the glyph (fixed text or a string subject), the hide
 * condition, the click callback and, optionally, the glyph label's name.
 */

// TEST_MIRROR_OK: the shipped code under test is ui_xml/components/preview_icon_button.xml

#include "../test_fixtures.h"

#include <cstring>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {

int s_clicks = 0;

void count_click(lv_event_t* /*e*/) {
    ++s_clicks;
}

lv_obj_t* glyph_of(lv_obj_t* button) {
    REQUIRE(button != nullptr);
    REQUIRE(lv_obj_get_child_count(button) == 1);
    lv_obj_t* label = lv_obj_get_child(button, 0);
    REQUIRE(lv_obj_check_type(label, &lv_label_class));
    return label;
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "preview_icon_button: one shell for fixed and bound glyphs",
                 "[ui][print_status][preview_icon_button]") {
    REQUIRE(register_component("components/preview_icon_button"));
    lv_xml_register_event_cb(nullptr, "on_test_preview_icon_click", count_click);
    s_clicks = 0;

    static lv_subject_t hide;
    lv_subject_init_int(&hide, 0);
    lv_xml_register_subject(nullptr, "test_preview_icon_hide", &hide);
    static char glyph_buf[16];
    static lv_subject_t glyph;
    lv_subject_init_string(&glyph, glyph_buf, nullptr, sizeof(glyph_buf), "A");
    lv_xml_register_subject(nullptr, "test_preview_icon_glyph", &glyph);

    const char* fixed_attrs[] = {"name",        "fixed_btn",
                                 "icon",        "#icon_webcam",
                                 "callback",    "on_test_preview_icon_click",
                                 "hidden_when", "test_preview_icon_hide eq 1",
                                 nullptr};
    lv_obj_t* fixed = create_component("preview_icon_button", fixed_attrs);
    const char* bound_attrs[] = {"name",         "bound_btn",
                                 "icon_subject", "test_preview_icon_glyph",
                                 "icon_name",    "bound_btn_icon",
                                 "callback",     "on_test_preview_icon_click",
                                 "hidden_when",  "test_preview_icon_hide eq 0",
                                 nullptr};
    lv_obj_t* bound = create_component("preview_icon_button", bound_attrs);
    REQUIRE(fixed != nullptr);
    REQUIRE(bound != nullptr);
    lv_obj_update_layout(lv_screen_active());

    SECTION("the shell") {
        for (lv_obj_t* btn : {fixed, bound}) {
            CHECK(lv_obj_get_width(btn) == 36);
            CHECK(lv_obj_get_height(btn) == 36);
            CHECK(lv_obj_get_style_bg_opa(btn, LV_PART_MAIN) == 180);
            CHECK(lv_obj_get_style_radius(btn, LV_PART_MAIN) == 18);
            CHECK(lv_obj_is_clickable(btn));
            CHECK_FALSE(lv_obj_is_scrollable(btn));
        }
    }

    SECTION("the glyph is fixed text or follows its subject") {
        CHECK(std::string(lv_label_get_text(glyph_of(fixed))) ==
              lv_xml_get_const(nullptr, "icon_webcam"));
        lv_obj_t* bound_glyph = glyph_of(bound);
        CHECK(lv_obj_find_by_name(bound, "bound_btn_icon") == bound_glyph);
        CHECK(std::string(lv_label_get_text(bound_glyph)) == "A");
        lv_subject_set_string(&glyph, "B");
        CHECK(std::string(lv_label_get_text(bound_glyph)) == "B");
    }

    SECTION("the hide condition and the callback are the host's") {
        CHECK_FALSE(lv_obj_is_hidden(fixed));
        CHECK(lv_obj_is_hidden(bound));
        lv_subject_set_int(&hide, 1);
        CHECK(lv_obj_is_hidden(fixed));
        CHECK_FALSE(lv_obj_is_hidden(bound));

        lv_obj_send_event(fixed, LV_EVENT_CLICKED, nullptr);
        lv_obj_send_event(bound, LV_EVENT_CLICKED, nullptr);
        CHECK(s_clicks == 2);
    }

    lv_obj_delete(fixed);
    lv_obj_delete(bound);
    lv_subject_deinit(&hide);
    lv_subject_deinit(&glyph);
}
