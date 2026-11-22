// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../test_helpers/xml_stand_in_subject.h"
#include "../ui_test_utils.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"
#include "test_fixtures.h"
#include "theme_manager.h"

#include "../catch_amalgamated.hpp"

namespace {

lv_obj_t* make(lv_obj_t* parent, const char* component, const char** attrs) {
    return static_cast<lv_obj_t*>(lv_xml_create(parent, component, attrs));
}

bool same_color(lv_color_t a, lv_color_t b) {
    return lv_color_to_u32(a) == lv_color_to_u32(b);
}

lv_color_t fill_of(lv_obj_t* pill) {
    return lv_obj_get_style_bg_color(pill, LV_PART_MAIN);
}

lv_color_t text_of(lv_obj_t* pill) {
    return lv_obj_get_style_text_color(pill, LV_PART_MAIN);
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "status_pill: a variant tints the fill and colors the text",
                 "[status_pill]") {
    SECTION("success") {
        const char* attrs[] = {"text", "Active", "variant", "success", nullptr};
        lv_obj_t* pill = make(test_screen(), "status_pill", attrs);
        REQUIRE(pill != nullptr);
        CHECK(UITest::get_text(pill) == "Active");
        CHECK(same_color(fill_of(pill), theme_manager_get_color("success")));
        CHECK(lv_obj_get_style_bg_opa(pill, LV_PART_MAIN) == 40);
        CHECK(same_color(text_of(pill), theme_manager_get_color("success")));
    }
    SECTION("muted by default") {
        const char* attrs[] = {"text", "Inactive", nullptr};
        lv_obj_t* pill = make(test_screen(), "status_pill", attrs);
        REQUIRE(pill != nullptr);
        CHECK(same_color(fill_of(pill), theme_manager_get_color("text_muted")));
        CHECK(lv_obj_get_style_bg_opa(pill, LV_PART_MAIN) == 40);
        CHECK(same_color(text_of(pill), theme_manager_get_color("text_muted")));
    }
}

TEST_CASE_METHOD(XMLTestFixture, "status_pill: bind_text shows an int subject as a number",
                 "[status_pill]") {
    lv_subject_t* count = helix::test::xml_stand_in("status_pill_test_count", 0);
    lv_subject_set_int(count, 3);
    const char* attrs[] = {"bind_text", "status_pill_test_count", nullptr};
    lv_obj_t* pill = make(test_screen(), "status_pill", attrs);
    REQUIRE(pill != nullptr);
    CHECK(UITest::get_text(pill) == "3");

    lv_subject_set_int(count, 11);
    process_lvgl(10);
    CHECK(UITest::get_text(pill) == "11");
}

TEST_CASE_METHOD(XMLTestFixture, "cal_intro_badge: neutral by default, warm is the soft amber pill",
                 "[status_pill]") {
    SECTION("neutral") {
        const char* attrs[] = {"text", "about 4 min", nullptr};
        lv_obj_t* badge = make(test_screen(), "cal_intro_badge", attrs);
        REQUIRE(badge != nullptr);
        CHECK(same_color(fill_of(badge), theme_manager_get_color("elevated_bg")));
        CHECK(same_color(text_of(badge), theme_manager_get_color("text_muted")));
        CHECK(lv_obj_get_style_radius(badge, LV_PART_MAIN) == 9999);
    }
    SECTION("warm") {
        const char* attrs[] = {"text", "loud", "variant", "warm", nullptr};
        lv_obj_t* badge = make(test_screen(), "cal_intro_badge", attrs);
        REQUIRE(badge != nullptr);
        CHECK(same_color(fill_of(badge), theme_manager_get_color("warning")));
        CHECK(lv_obj_get_style_bg_opa(badge, LV_PART_MAIN) < 40);
        CHECK(same_color(text_of(badge), theme_manager_get_color("warning")));
    }
}

TEST_CASE_METHOD(XMLTestFixture,
                 "ams_disconnected_chip: a round warning pill shown while its subject is 1",
                 "[status_pill]") {
    lv_subject_t* offline = helix::test::xml_stand_in("status_pill_test_offline", 0);
    const char* attrs[] = {"disconnected", "status_pill_test_offline", nullptr};
    lv_obj_t* chip = make(test_screen(), "ams_disconnected_chip", attrs);
    REQUIRE(chip != nullptr);
    CHECK(UITest::get_text(chip) == "Disconnected");
    CHECK(same_color(fill_of(chip), theme_manager_get_color("warning")));
    CHECK(same_color(text_of(chip), theme_manager_get_color("warning")));
    CHECK(lv_obj_get_style_radius(chip, LV_PART_MAIN) == 9999);
    CHECK(lv_obj_is_hidden(chip));

    lv_subject_set_int(offline, 1);
    process_lvgl(10);
    CHECK_FALSE(lv_obj_is_hidden(chip));
}

TEST_CASE_METHOD(XMLTestFixture, "tag_badge: a round info pill shown while its subject is non-zero",
                 "[status_pill]") {
    lv_subject_t* from_tag = helix::test::xml_stand_in("status_pill_test_from_tag", 0);
    const char* attrs[] = {"subject", "status_pill_test_from_tag", nullptr};
    lv_obj_t* badge = make(test_screen(), "tag_badge", attrs);
    REQUIRE(badge != nullptr);
    CHECK(UITest::get_text(badge) == "from tag");
    CHECK(same_color(fill_of(badge), theme_manager_get_color("info")));
    CHECK(same_color(text_of(badge), theme_manager_get_color("info")));
    CHECK(lv_obj_is_hidden(badge));

    lv_subject_set_int(from_tag, 2);
    process_lvgl(10);
    CHECK_FALSE(lv_obj_is_hidden(badge));
}
