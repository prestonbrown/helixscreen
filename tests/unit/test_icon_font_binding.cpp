// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_icon_font_binding.cpp
 * @brief An icon's face must be overridable by a bound style, and every
 *        compiled MDI face must be recognised as an icon font.
 *
 * theme_manager's icon-font predicate gates the "leave icons alone" branch of
 * the theme-change walk. A face it does not recognise falls through into code
 * that writes inline colours, which override the icon variant styles and
 * HeatingIconAnimator's tint.
 *
 * Which faces exist is a build-time question: HELIX_MAX_FONT_TIER says how far
 * up the ladder this build linked, and naming a face above it does not link.
 *
 * Run with: ./build/bin/helix-tests "[icon][font]"
 */

#include "ui_fonts.h"

#include "../test_fixtures.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_component.h"
#include "lvgl/lvgl.h"
#include "theme_manager.h"

#include <vector>

#include "../catch_amalgamated.hpp"

TEST_CASE("every compiled MDI face is recognised as an icon font", "[icon][font][theme]") {
    // globals.xml resolves icon_font_lg_xxlarge to mdi_icons_80 and
    // icon_font_xl_xxlarge to mdi_icons_96, so a predicate that stops at 64
    // mis-classifies icons that ship today.
    struct Face {
        const char* name;
        const lv_font_t* font;
    };
    const std::vector<Face> faces = {
        {"mdi_icons_14", &mdi_icons_14}, {"mdi_icons_16", &mdi_icons_16},
        {"mdi_icons_24", &mdi_icons_24}, {"mdi_icons_32", &mdi_icons_32},
        {"mdi_icons_48", &mdi_icons_48}, {"mdi_icons_64", &mdi_icons_64},
#if HELIX_MAX_FONT_TIER >= 5
        {"mdi_icons_80", &mdi_icons_80},
#endif
#if HELIX_MAX_FONT_TIER >= 6
        {"mdi_icons_96", &mdi_icons_96}, {"mdi_icons_128", &mdi_icons_128},
#endif
    };

    for (const auto& f : faces) {
        INFO("face " << f.name);
        CHECK(helix::ui::is_icon_font(f.font));
    }
}

TEST_CASE("a text face is not an icon font", "[icon][font][theme]") {
    // The predicate is a guard, so a false positive silently skips the colour
    // pass for real text; assert it discriminates rather than returning true.
    CHECK_FALSE(helix::ui::is_icon_font(&noto_sans_14));
    CHECK_FALSE(helix::ui::is_icon_font(nullptr));
}

namespace {

constexpr const char* ICON_MODE_SUBJECT = "icon_font_test_mode";

/// The four ways an <icon>'s face can be decided: a size attribute, a bound
/// style, the create-path default when no size is given, and an inline
/// attribute. The default_icon case matters because ui_icon_xml_create applies
/// a face unconditionally, so a fix that only covers the attribute path leaves
/// that one writing locally.
constexpr const char* ICON_FIXTURE_XML = R"(<component>
  <styles>
    <style name="big_icon" text_font="#icon_font_xl"/>
    <style name="tiny_icon" text_font="#icon_font_xs"/>
  </styles>
  <view name="root" extends="lv_obj">
    <icon name="bound_icon" src="power" size="sm">
      <bind_style_if_eq name="big_icon" subject="icon_font_test_mode" ref_value="1"/>
    </icon>
    <icon name="plain_icon" src="power" size="sm"/>
    <icon name="default_icon" src="power">
      <bind_style_if_eq name="tiny_icon" subject="icon_font_test_mode" ref_value="1"/>
    </icon>
    <icon name="inline_icon" src="power" size="sm" style_text_font="#icon_font_xl"/>
  </view>
</component>)";

lv_subject_t& icon_mode_subject() {
    static lv_subject_t subject;
    static bool registered = false;
    if (!registered) {
        lv_subject_init_int(&subject, 0);
        lv_xml_register_subject(nullptr, ICON_MODE_SUBJECT, &subject);
        registered = true;
    }
    return subject;
}

lv_obj_t* create_icon_fixture(lv_obj_t* parent) {
    static bool registered = false;
    if (!registered) {
        REQUIRE(lv_xml_register_component_from_data("icon_font_fixture", ICON_FIXTURE_XML) ==
                LV_RESULT_OK);
        registered = true;
    }
    return static_cast<lv_obj_t*>(lv_xml_create(parent, "icon_font_fixture", nullptr));
}

const lv_font_t* icon_rung(const char* token) {
    const lv_font_t* font = theme_manager_get_font(token);
    REQUIRE(font != nullptr);
    return font;
}

} // namespace

TEST_CASE("a bound style overrides an icon's face", "[xml][icon][font][1559]") {
    XMLTestFixture fixture;
    auto& mode = icon_mode_subject();

    const lv_font_t* sm = icon_rung("icon_font_sm");
    const lv_font_t* xl = icon_rung("icon_font_xl");
    // The rungs must resolve to different faces or the assertions below cannot
    // tell the size attribute's face from the bound one.
    REQUIRE(sm != xl);

    lv_obj_t* root = create_icon_fixture(fixture.test_screen());
    REQUIRE(root != nullptr);
    lv_obj_t* bound = lv_obj_find_by_name(root, "bound_icon");
    lv_obj_t* plain = lv_obj_find_by_name(root, "plain_icon");
    REQUIRE(bound != nullptr);
    REQUIRE(plain != nullptr);

    SECTION("the subject selects the bound face, and de-selecting restores the rung") {
        lv_subject_set_int(&mode, 1);
        CHECK(lv_obj_get_style_text_font(bound, LV_PART_MAIN) == xl);

        lv_subject_set_int(&mode, 0);
        CHECK(lv_obj_get_style_text_font(bound, LV_PART_MAIN) == sm);
    }

    SECTION("an icon with no face bind keeps its size attribute's rung") {
        lv_subject_set_int(&mode, 1);
        CHECK(lv_obj_get_style_text_font(plain, LV_PART_MAIN) == sm);
    }
}

TEST_CASE("an icon with no size attribute still accepts a bound face", "[xml][icon][font][1559]") {
    // ui_icon_xml_create applies a face whether or not size= is present, so
    // this path has to stop writing locally too.
    XMLTestFixture fixture;
    auto& mode = icon_mode_subject();

    const lv_font_t* xs = icon_rung("icon_font_xs");
    const lv_font_t* xl = icon_rung("icon_font_xl");
    REQUIRE(xs != xl);

    lv_obj_t* root = create_icon_fixture(fixture.test_screen());
    REQUIRE(root != nullptr);
    lv_obj_t* def = lv_obj_find_by_name(root, "default_icon");
    REQUIRE(def != nullptr);

    lv_subject_set_int(&mode, 0);
    CHECK(lv_obj_get_style_text_font(def, LV_PART_MAIN) == xl);

    lv_subject_set_int(&mode, 1);
    CHECK(lv_obj_get_style_text_font(def, LV_PART_MAIN) == xs);
}

TEST_CASE("an inline style_text_font still outranks an icon's bound face",
          "[xml][icon][font][1559]") {
    XMLTestFixture fixture;
    auto& mode = icon_mode_subject();

    const lv_font_t* sm = icon_rung("icon_font_sm");
    const lv_font_t* xl = icon_rung("icon_font_xl");
    REQUIRE(sm != xl);

    lv_obj_t* root = create_icon_fixture(fixture.test_screen());
    REQUIRE(root != nullptr);
    lv_obj_t* inline_icon = lv_obj_find_by_name(root, "inline_icon");
    REQUIRE(inline_icon != nullptr);

    // Local on both sides of the subject: the inline attribute is evaluated
    // once at parse time and must hold regardless of any bound style's state.
    lv_subject_set_int(&mode, 1);
    CHECK(lv_obj_get_style_text_font(inline_icon, LV_PART_MAIN) == xl);
    lv_subject_set_int(&mode, 0);
    CHECK(lv_obj_get_style_text_font(inline_icon, LV_PART_MAIN) == xl);

    // And it is genuinely overriding a different rung, not echoing it.
    CHECK(lv_obj_get_style_text_font(lv_obj_find_by_name(root, "plain_icon"), LV_PART_MAIN) == sm);
}

namespace {

/// Same shape as nozzle_icon/heater_icon: a component whose rung prop defaults
/// to empty, so a call site that does not size itself resolves the binding's
/// subject to "". The app registers an int-0 subject under that empty name
/// (src/xml_registration.cpp), so without an engine-side skip the eq-0 rung
/// style installs and pins the glyph to the xs face at every breakpoint.
constexpr const char* EMPTY_RUNG_FIXTURE_XML = R"(<component>
  <api>
    <prop name="rung_subject" type="string" default=""/>
  </api>
  <styles>
    <style name="xs_rung" text_font="#icon_font_xs"/>
  </styles>
  <view name="root" extends="lv_obj">
    <icon name="unsized_icon" src="power" size="sm">
      <bind_style_if_eq name="xs_rung" subject="$rung_subject" ref_value="0"/>
    </icon>
  </view>
</component>)";

void ensure_empty_name_subject() {
    // Mirrors the app's global "" noop subject: int 0, registered under the
    // empty name. Without this the binding would resolve NULL and skip, and
    // the test could not tell a guarded skip from a failed lookup.
    static lv_subject_t subject;
    static bool registered = false;
    if (!registered) {
        lv_subject_init_int(&subject, 0);
        lv_xml_register_subject(nullptr, "", &subject);
        registered = true;
    }
}

} // namespace

TEST_CASE("a rung bind left at its empty default installs no style", "[xml][icon][font]") {
    XMLTestFixture fixture;
    ensure_empty_name_subject();

    const lv_font_t* sm = icon_rung("icon_font_sm");
    const lv_font_t* xs = icon_rung("icon_font_xs");
    REQUIRE(sm != xs);

    REQUIRE(lv_xml_register_component_from_data("icon_empty_rung_fixture",
                                                EMPTY_RUNG_FIXTURE_XML) == LV_RESULT_OK);
    lv_obj_t* root = static_cast<lv_obj_t*>(
        lv_xml_create(fixture.test_screen(), "icon_empty_rung_fixture", nullptr));
    REQUIRE(root != nullptr);
    lv_obj_t* unsized = lv_obj_find_by_name(root, "unsized_icon");
    REQUIRE(unsized != nullptr);

    CHECK(lv_obj_get_style_text_font(unsized, LV_PART_MAIN) == sm);
}
