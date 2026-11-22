// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_choice_button.cpp
 * @brief choice_button: the checked state follows its subject, each look
 *        resolves to its styles.choice_<look>_* pair, and a tap hands the
 *        callback its user_data.
 */

#include "../test_fixtures.h"
#include "core/subject.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"
#include "theme_manager.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

std::vector<std::string> g_taps;

void record_tap(lv_event_t* e) {
    const char* data = static_cast<const char*>(lv_event_get_user_data(e));
    g_taps.emplace_back(data ? data : "<null>");
}

class ChoiceButtonFixture : public XMLTestFixture {
  public:
    ChoiceButtonFixture() {
        pick_.init_int(0);
        lv_xml_register_subject(nullptr, "test_choice_pick", &pick_);
        lv_xml_register_event_cb(nullptr, "test_choice_tap", record_tap);
        REQUIRE(lv_xml_register_component_from_file("A:ui_xml/components/choice_button.xml") ==
                LV_RESULT_OK);
        g_taps.clear();
    }

    ~ChoiceButtonFixture() override {
        lv_xml_unregister_subject(nullptr, "test_choice_pick");
        pick_.reset();
    }

    lv_obj_t* make(const char* value, const char* look, const char* variant = "primary") {
        const char* attrs[] = {"subject",   "test_choice_pick",
                               "value",     value,
                               "user_data", value,
                               "callback",  "test_choice_tap",
                               "look",      look,
                               "variant",   variant,
                               "text",      "Pick",
                               nullptr};
        lv_obj_t* btn = create_component("choice_button", attrs);
        REQUIRE(btn != nullptr);
        return btn;
    }

    void pick(int v) {
        lv_subject_set_int(pick_.get(), v);
    }

    helix::Subject pick_{};
};

bool checked(lv_obj_t* obj) {
    return lv_obj_has_state(obj, LV_STATE_CHECKED);
}

} // namespace

TEST_CASE_METHOD(ChoiceButtonFixture, "choice_button is checked while its subject holds its value",
                 "[choice_button][xml]") {
    lv_obj_t* first = make("0", "fill");
    lv_obj_t* second = make("1", "fill");

    CHECK(checked(first));
    CHECK_FALSE(checked(second));

    pick(1);
    CHECK_FALSE(checked(first));
    CHECK(checked(second));

    pick(2);
    CHECK_FALSE(checked(first));
    CHECK_FALSE(checked(second));
}

TEST_CASE_METHOD(ChoiceButtonFixture, "choice_button wears its look's idle and selected styles",
                 "[choice_button][xml]") {
    SECTION("fill: solid when chosen, a primary wash otherwise") {
        lv_obj_t* chosen = make("0", "fill");
        lv_obj_t* idle = make("1", "fill");
        CHECK(lv_obj_get_style_bg_opa(chosen, LV_PART_MAIN) == 255);
        CHECK(lv_obj_get_style_bg_opa(idle, LV_PART_MAIN) == 40);
        CHECK(lv_color_eq(lv_obj_get_style_bg_color(idle, LV_PART_MAIN),
                          theme_manager_get_color("primary")));
    }

    SECTION("raised: idle sits on the raised surface") {
        lv_obj_t* chosen = make("0", "raised");
        lv_obj_t* idle = make("1", "raised");
        CHECK(lv_color_eq(lv_obj_get_style_bg_color(chosen, LV_PART_MAIN),
                          theme_manager_get_color("primary")));
        CHECK(lv_color_eq(lv_obj_get_style_bg_color(idle, LV_PART_MAIN),
                          theme_manager_get_color("elevated_bg")));
        CHECK(lv_obj_get_style_bg_opa(idle, LV_PART_MAIN) == 255);
    }

    SECTION("outline: the chosen one drops its frame") {
        lv_obj_t* chosen = make("0", "outline", "outline");
        lv_obj_t* idle = make("1", "outline", "outline");
        CHECK(lv_obj_get_style_border_width(chosen, LV_PART_MAIN) == 0);
        CHECK(lv_obj_get_style_bg_opa(chosen, LV_PART_MAIN) == 255);
        CHECK(lv_color_eq(lv_obj_get_style_border_color(idle, LV_PART_MAIN),
                          theme_manager_get_color("elevated_bg")));
    }

    SECTION("tab: the chosen tab takes a primary tint, the rest stay clear") {
        lv_obj_t* chosen = make("0", "tab", "ghost");
        lv_obj_t* idle = make("1", "tab", "ghost");
        CHECK(lv_obj_get_style_bg_opa(chosen, LV_PART_MAIN) == 30);
        CHECK(lv_obj_get_style_bg_opa(idle, LV_PART_MAIN) == 0);
    }

    SECTION("the selection moves with the subject") {
        lv_obj_t* first = make("0", "fill");
        pick(1);
        CHECK(lv_obj_get_style_bg_opa(first, LV_PART_MAIN) == 40);
        pick(0);
        CHECK(lv_obj_get_style_bg_opa(first, LV_PART_MAIN) == 255);
    }
}

TEST_CASE_METHOD(ChoiceButtonFixture, "choice_button calls its callback with its user_data",
                 "[choice_button][xml]") {
    lv_obj_t* second = make("1", "fill");
    lv_obj_send_event(second, LV_EVENT_CLICKED, nullptr);
    REQUIRE(g_taps.size() == 1);
    CHECK(g_taps[0] == "1");
}
