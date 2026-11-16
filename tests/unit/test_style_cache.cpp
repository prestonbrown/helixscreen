// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_style_cache.cpp
 * @brief A property added to a style already on a widget reaches that widget.
 *
 * With LV_OBJ_STYLE_CACHE each widget keeps two masks of which properties its
 * styles can hold (one for MAIN, one for every other part), and a lookup skips a
 * property the mask lacks. Adding a property to a style that is already attached
 * leaves the mask stale until lv_obj_report_style_change() rebuilds it, so the
 * widget silently reads the default. Each case below adds a property in a mask
 * group nothing else on the widget sets, so only a rebuilt mask can find it.
 */

#include "../lvgl_test_fixture.h"
#include "helix-xml/src/xml/lv_xml_component.h"
#include "helix-xml/src/xml/lv_xml_style.h"
#include "theme_manager.h"

#include "../catch_amalgamated.hpp"

static_assert(LV_OBJ_STYLE_CACHE, "these cases exercise the style cache; lv_conf.h turns it off");

namespace {

/// A widget carrying no theme or local style, so only the styles a case adds set its masks.
lv_obj_t* bare_widget(lv_obj_t* parent) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    return obj;
}

bool style_has(const lv_style_t* style, lv_style_prop_t prop) {
    lv_style_value_t v;
    return lv_style_get_prop(style, prop, &v) == LV_STYLE_RES_FOUND;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "style cache: a report reaches every part a style sits on",
                 "[style_cache]") {
    lv_style_t shared;
    lv_style_init(&shared);
    lv_style_set_bg_opa(&shared, LV_OPA_COVER);

    lv_obj_t* obj = bare_widget(test_screen());
    lv_obj_add_style(obj, &shared, LV_PART_MAIN);
    lv_obj_add_style(obj, &shared, LV_PART_INDICATOR);
    REQUIRE(lv_obj_get_style_margin_left(obj, LV_PART_MAIN) == 0);
    REQUIRE(lv_obj_get_style_margin_left(obj, LV_PART_INDICATOR) == 0);

    lv_style_set_margin_left(&shared, 7);
    lv_obj_report_style_change(&shared);

    CHECK(lv_obj_get_style_margin_left(obj, LV_PART_MAIN) == 7);
    CHECK(lv_obj_get_style_margin_left(obj, LV_PART_INDICATOR) == 7);

    lv_obj_delete(obj);
    lv_style_reset(&shared);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "style cache: a theme rebuild that adds a property reaches widgets wearing it",
                 "[style_cache][theme-manager]") {
    auto& tm = ThemeManager::instance();
    tm.init();
    tm.set_glass(false);
    // shadow_width 0: the Card style holds no shadow until glass adds one.
    ThemePalette flat{};
    flat.shadow_width = 0;
    tm.set_palettes(flat, flat);

    lv_style_t* card = tm.get_style(StyleRole::Card);
    REQUIRE_FALSE(style_has(card, LV_STYLE_SHADOW_OFFSET_Y));

    lv_obj_t* obj = bare_widget(test_screen());
    lv_obj_add_style(obj, card, LV_PART_MAIN);
    REQUIRE(lv_obj_get_style_shadow_offset_y(obj, LV_PART_MAIN) == 0);

    tm.set_glass(true);
    REQUIRE(style_has(card, LV_STYLE_SHADOW_OFFSET_Y));

    CHECK(lv_obj_get_style_shadow_offset_y(obj, LV_PART_MAIN) == 4); // kGlassSurfaceShadow

    lv_obj_delete(obj);
    tm.set_glass(false);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "style cache: extending a registered XML style reaches widgets wearing it",
                 "[style_cache][xml]") {
    REQUIRE(lv_xml_register_component_from_data("globals", R"(<component><styles>
                    <style name="style_cache_probe" bg_opa="255"/>
                </styles></component>)") == LV_RESULT_OK);
    lv_xml_style_t* probe =
        lv_xml_get_style_by_name(lv_xml_component_get_scope("globals"), "style_cache_probe");
    REQUIRE(probe != nullptr);
    // A second run in one process finds the property left by the first.
    lv_style_remove_prop(&probe->style, LV_STYLE_MARGIN_RIGHT);

    lv_obj_t* obj = bare_widget(test_screen());
    lv_obj_add_style(obj, &probe->style, LV_PART_MAIN);
    REQUIRE(lv_obj_get_style_margin_right(obj, LV_PART_MAIN) == 0);

    // The same name again extends the style in place rather than creating a new one.
    REQUIRE(lv_xml_register_component_from_data("globals", R"(<component><styles>
                    <style name="style_cache_probe" margin_right="5"/>
                </styles></component>)") == LV_RESULT_OK);
    REQUIRE(lv_xml_get_style_by_name(lv_xml_component_get_scope("globals"), "style_cache_probe") ==
            probe);

    CHECK(lv_obj_get_style_margin_right(obj, LV_PART_MAIN) == 5);

    lv_obj_delete(obj);
}
