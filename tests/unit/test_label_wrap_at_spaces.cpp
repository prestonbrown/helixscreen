// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
// TEST_MIRROR_OK: drives lib/lvgl as patched by patches/lvgl_label_wrap_at_spaces.patch and the
// helix-xml label parser
//
// A label set to wrap at spaces (lv_label_set_wrap_at_spaces, XML wrap_at_spaces)
// keeps "[save_variables]" whole instead of breaking it at its '_', and the height
// the label measures for itself matches the lines it lays out.

#include "../lvgl_test_fixture.h"
#include "../test_fixtures.h"
#include "helix-xml/src/xml/lv_xml_component.h"

#include "../catch_amalgamated.hpp"

namespace {

constexpr const char* kText = "Please add the [save_variables] section";
// Byte offsets into kText.
constexpr uint32_t kOpenBracket = 15; // '[' of "[save_variables]"
constexpr uint32_t kVariables = 21;   // 'v' right after the '_'
constexpr uint32_t kSection = 32;     // 's' of "section"

int32_t text_width(const lv_font_t* font, const char* txt) {
    lv_point_t size = {0, 0};
    lv_text_get_size(&size, txt, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_EXPAND);
    return size.x;
}

lv_point_t letter_pos(lv_obj_t* label, uint32_t byte_id) {
    lv_point_t p = {0, 0};
    lv_label_get_letter_pos(label, byte_id, &p);
    return p;
}

// A wrapping label exactly as wide as "Please add the [save_": the stock rule
// breaks there, mid-word; wrapping at spaces moves the whole word down instead.
lv_obj_t* make_label(lv_obj_t* parent) {
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_style_pad_all(label, 0, LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    const lv_font_t* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    lv_obj_set_width(label, text_width(font, "Please add the [save_"));
    lv_obj_set_height(label, LV_SIZE_CONTENT);
    lv_label_set_text(label, kText);
    lv_obj_update_layout(label);
    return label;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "A label wrapping at spaces keeps [save_variables] on one line",
                 "[label][wrap_at_spaces]") {
    lv_obj_t* label = make_label(test_screen());
    const lv_font_t* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    const int32_t line_h = lv_font_get_line_height(font);
    const int32_t w = lv_obj_get_content_width(label);

    // The width sits where the two rules disagree.
    REQUIRE(text_width(font, "Please add the [save_v") > w);
    REQUIRE(text_width(font, "[save_variables]") <= w);
    REQUIRE(text_width(font, "[save_variables] section") > w);
    REQUIRE(text_width(font, "variables] section") <= w);

    SECTION("stock wrapping breaks at the underscore") {
        CHECK(lv_obj_get_height(label) == 2 * line_h);
        CHECK(letter_pos(label, kOpenBracket).y == 0);
        CHECK(letter_pos(label, kVariables).x == 0);
        CHECK(letter_pos(label, kVariables).y == line_h);
        CHECK(letter_pos(label, kSection).y == line_h);
    }

    SECTION("wrapping at spaces moves the whole word down") {
        lv_label_set_wrap_at_spaces(label, true);
        lv_obj_update_layout(label);

        CHECK(lv_obj_get_height(label) == 3 * line_h);
        CHECK(letter_pos(label, kOpenBracket).x == 0);
        CHECK(letter_pos(label, kOpenBracket).y == line_h);
        CHECK(letter_pos(label, kVariables).y == line_h);
        CHECK(letter_pos(label, kSection).x == 0);
        CHECK(letter_pos(label, kSection).y == 2 * line_h);
    }

    SECTION("turning it back off restores the stock layout") {
        lv_label_set_wrap_at_spaces(label, true);
        lv_obj_update_layout(label);
        lv_label_set_wrap_at_spaces(label, false);
        lv_obj_update_layout(label);

        CHECK(lv_obj_get_height(label) == 2 * line_h);
        CHECK(letter_pos(label, kVariables).x == 0);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "A word wider than a space-wrapping label still breaks",
                 "[label][wrap_at_spaces]") {
    lv_obj_t* label = lv_label_create(test_screen());
    lv_obj_set_style_pad_all(label, 0, LV_PART_MAIN);
    const lv_font_t* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    const int32_t line_h = lv_font_get_line_height(font);
    lv_obj_set_width(label, text_width(font, "[save_var"));
    lv_obj_set_height(label, LV_SIZE_CONTENT);
    lv_label_set_wrap_at_spaces(label, true);
    lv_label_set_text(label, "[save_variables]");
    lv_obj_update_layout(label);

    REQUIRE(text_width(font, "[save_vari") > lv_obj_get_content_width(label));
    CHECK(lv_obj_get_height(label) == 2 * line_h);
    CHECK(letter_pos(label, 9).x == 0); // 'i': the first letter that did not fit
    CHECK(letter_pos(label, 9).y == line_h);
}

TEST_CASE_METHOD(XMLTestFixture, "wrap_at_spaces in XML sets the label's wrap rule",
                 "[label][wrap_at_spaces][xml]") {
    const char* xml = R"(<component>
  <view extends="lv_obj" width="400" height="300">
    <lv_label name="plain_on" text="x" wrap_at_spaces="true"/>
    <text_body name="body_on" text="x" wrap_at_spaces="true"/>
    <text_small name="small_off" text="x" wrap_at_spaces="false"/>
    <text_body name="body_default" text="x"/>
  </view>
</component>)";
    REQUIRE(lv_xml_register_component_from_data("wrap_at_spaces_probe", xml) == LV_RESULT_OK);
    lv_obj_t* root = create_component("wrap_at_spaces_probe");
    REQUIRE(root != nullptr);

    CHECK(lv_label_get_wrap_at_spaces(lv_obj_find_by_name(root, "plain_on")));
    CHECK(lv_label_get_wrap_at_spaces(lv_obj_find_by_name(root, "body_on")));
    CHECK_FALSE(lv_label_get_wrap_at_spaces(lv_obj_find_by_name(root, "small_off")));
    CHECK_FALSE(lv_label_get_wrap_at_spaces(lv_obj_find_by_name(root, "body_default")));
}
