// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_header_action.cpp
 * @brief header_action, the one button a header's actions slot takes.
 *
 * The component owns the look: a circle when it shows only its icon, a pill
 * with text, the fill by role, and the collapse to its icon on the micro and
 * tiny breakpoints. Bindings written as its children act on the button.
 */

#include "ui_button.h"

#include "../lvgl_test_fixture.h"
#include "../test_fixtures.h"
#include "../test_helpers/scoped_breakpoint.h"
#include "core/subject.h"
#include "theme_manager.h"

#include <lvgl.h>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::test::ScopedBreakpoint;

namespace {

constexpr lv_state_t kIconOnly = LV_STATE_USER_4;

struct HeaderActionFixture : public XMLTestFixture {
    lv_obj_t* make(std::initializer_list<const char*> kv) {
        std::vector<const char*> attrs(kv);
        attrs.push_back(nullptr);
        lv_obj_t* btn = create_component("header_action", attrs.data());
        REQUIRE(btn != nullptr);
        lv_obj_update_layout(btn);
        return btn;
    }
};

bool same_color(lv_color_t a, lv_color_t b) {
    return lv_color_eq(a, b);
}

} // namespace

TEST_CASE_METHOD(HeaderActionFixture, "header_action with only an icon is a circle",
                 "[header_action][xml]") {
    ScopedBreakpoint bp(UiBreakpoint::Medium);
    lv_obj_t* btn = make({"icon", "refresh"});

    const int32_t h = theme_manager_get_spacing("header_button_height");
    CHECK(lv_obj_has_state(btn, kIconOnly));
    CHECK(lv_obj_get_height(btn) == h);
    CHECK(lv_obj_get_width(btn) == h);
    CHECK(lv_obj_get_style_radius(btn, LV_PART_MAIN) >= h / 2);
}

TEST_CASE_METHOD(HeaderActionFixture, "header_action with text is a pill", "[header_action][xml]") {
    ScopedBreakpoint bp(UiBreakpoint::Medium);
    lv_obj_t* btn = make({"icon", "plus", "text", "Add Spool"});

    const int32_t h = theme_manager_get_spacing("header_button_height");
    CHECK_FALSE(lv_obj_has_state(btn, kIconOnly));
    CHECK(lv_obj_get_height(btn) == h);
    CHECK(lv_obj_get_width(btn) > h);
    CHECK(lv_obj_get_style_radius(btn, LV_PART_MAIN) >= h / 2);
    CHECK(lv_obj_get_style_pad_left(btn, LV_PART_MAIN) > 0);
}

TEST_CASE_METHOD(HeaderActionFixture, "header_action fills by role", "[header_action][xml]") {
    lv_obj_t* neutral = make({"text", "Refresh"});
    lv_obj_t* primary = make({"text", "Save", "role", "primary"});
    lv_obj_t* danger = make({"text", "Clear All", "role", "danger"});

    CHECK(same_color(lv_obj_get_style_bg_color(neutral, LV_PART_MAIN),
                     theme_manager_get_color("elevated_bg")));
    CHECK(same_color(lv_obj_get_style_bg_color(primary, LV_PART_MAIN),
                     theme_manager_get_color("primary")));
    CHECK(same_color(lv_obj_get_style_bg_color(danger, LV_PART_MAIN),
                     theme_manager_get_color("danger")));
}

TEST_CASE_METHOD(HeaderActionFixture,
                 "header_action collapses to its icon on micro, tiny and narrow portrait",
                 "[header_action][xml]") {
    ScopedBreakpoint bp(UiBreakpoint::Medium);
    lv_obj_t* both = make({"icon", "script_text", "text", "Printer Events"});
    lv_obj_t* kept = make({"icon", "script_text", "text", "Printer Events", "collapse", "never"});
    lv_obj_t* text_only = make({"text", "Clear All"});
    lv_obj_t* label = ui_button_get_label(both);
    REQUIRE(label != nullptr);

    CHECK_FALSE(lv_obj_is_hidden(label));
    CHECK_FALSE(lv_obj_has_state(both, kIconOnly));

    {
        ScopedBreakpoint tiny(UiBreakpoint::Tiny);
        lv_obj_update_layout(both);
        CHECK(lv_obj_is_hidden(label));
        CHECK(lv_obj_has_state(both, kIconOnly));
        CHECK(lv_obj_get_width(both) == lv_obj_get_height(both));
        // collapse="never" keeps its text; with no icon there is nothing to collapse to.
        CHECK_FALSE(lv_obj_is_hidden(ui_button_get_label(kept)));
        CHECK_FALSE(lv_obj_is_hidden(ui_button_get_label(text_only)));
        CHECK_FALSE(lv_obj_has_state(text_only, kIconOnly));
    }

    CHECK_FALSE(lv_obj_is_hidden(label));
    CHECK_FALSE(lv_obj_has_state(both, kIconOnly));

    // A portrait header is as narrow as a micro one's below the large breakpoint.
    lv_subject_t* portrait = lv_xml_get_subject(nullptr, "ui_is_portrait");
    REQUIRE(portrait != nullptr);
    const int32_t was_portrait = lv_subject_get_int(portrait);
    lv_subject_set_int(portrait, 1);
    CHECK(lv_obj_is_hidden(label));
    {
        ScopedBreakpoint large(UiBreakpoint::Large);
        CHECK_FALSE(lv_obj_is_hidden(label));
    }
    lv_subject_set_int(portrait, was_portrait);
    CHECK_FALSE(lv_obj_is_hidden(label));
}

TEST_CASE_METHOD(HeaderActionFixture, "header_action child bindings act on the button",
                 "[header_action][xml]") {
    static helix::Subject hide{};
    static helix::Subject busy{};
    hide.init_int(0);
    busy.init_int(0);
    lv_xml_register_subject(nullptr, "test_header_action_hide", &hide);
    lv_xml_register_subject(nullptr, "test_header_action_busy", &busy);

    REQUIRE(lv_xml_register_component_from_data(
                "header_action_host",
                "<component><view extends=\"lv_obj\">"
                "<header_action name=\"btn_bound\" icon=\"refresh\" text=\"Refresh\">"
                "<bind_flag_if_eq subject=\"test_header_action_hide\" flag=\"hidden\" "
                "ref_value=\"1\"/>"
                "<bind_state_if_eq subject=\"test_header_action_busy\" state=\"disabled\" "
                "ref_value=\"1\"/>"
                "</header_action>"
                "</view></component>") == LV_RESULT_OK);
    lv_obj_t* host = create_component("header_action_host");
    REQUIRE(host != nullptr);
    lv_obj_t* btn = lv_obj_find_by_name(host, "btn_bound");
    REQUIRE(btn != nullptr);
    REQUIRE(lv_obj_get_parent(btn) == host);

    CHECK_FALSE(lv_obj_is_hidden(btn));
    CHECK_FALSE(lv_obj_has_state(btn, LV_STATE_DISABLED));
    lv_subject_set_int(&hide, 1);
    CHECK(lv_obj_is_hidden(btn));
    lv_subject_set_int(&busy, 1);
    CHECK(lv_obj_has_state(btn, LV_STATE_DISABLED));

    lv_obj_delete(host);
    hide.reset();
    busy.reset();
}

TEST_CASE_METHOD(HeaderActionFixture,
                 "header_action touch targets stay apart and inside the header on small screens",
                 "[header_action][xml][touch]") {
    REQUIRE(register_component("header_bar"));
    REQUIRE(lv_xml_register_component_from_data(
                "header_action_row",
                "<component><view extends=\"lv_obj\" width=\"100%\" height=\"100%\">"
                "<header_bar title=\"Spoolman\"><header_bar-actions>"
                "<header_action name=\"a\" icon=\"plus\" text=\"Add\" role=\"primary\"/>"
                "<header_action name=\"b\" icon=\"refresh\"/>"
                "<header_action name=\"c\" text=\"Clear All\"/>"
                "</header_bar-actions></header_bar>"
                "</view></component>") == LV_RESULT_OK);

    struct Size {
        int32_t w;
        int32_t h;
    };
    for (const Size size : {Size{480, 272}, Size{480, 320}, Size{800, 480}}) {
        INFO(size.w << "x" << size.h);
        ScopedResolution res(lv_display_get_default(), size.w, size.h);
        theme_manager_refresh_layout_constants(lv_display_get_default());
        lv_obj_t* row = create_component("header_action_row");
        REQUIRE(row != nullptr);
        lv_obj_update_layout(row);

        lv_obj_t* host = lv_obj_find_by_name(row, "header_actions");
        REQUIRE(host != nullptr);
        lv_area_t parent;
        lv_obj_get_coords(host, &parent);
        lv_area_t prev{};
        bool first = true;
        for (const char* name : {"a", "b", "c"}) {
            INFO(name);
            lv_obj_t* btn = lv_obj_find_by_name(row, name);
            REQUIRE(btn != nullptr);
            lv_area_t hit;
            lv_obj_get_click_area(btn, &hit);
            // LVGL hit-tests a child only inside its parent, so a reach past
            // the slot's edge would never register.
            CHECK(hit.x1 >= parent.x1);
            CHECK(hit.x2 <= parent.x2);
            CHECK(hit.y1 >= parent.y1);
            CHECK(hit.y2 <= parent.y2);
            if (!first) {
                CHECK(prev.x2 < hit.x1);
            }
            prev = hit;
            first = false;
        }
        lv_obj_delete(row);
    }
}
