// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_semantic_font_binding.cpp
 * @brief A text_font bound from XML must be able to override a semantic text
 *        widget's own font.
 *
 * apply_semantic_font() writes each text_* widget's font as an LVGL LOCAL
 * style, which outranks every ADDED style — and the XML engine applies
 * bind_style / bind_style_if_* through lv_obj_add_style — so a style carrying
 * text_font bound onto a text_small / text_body / ... was silently ignored and
 * the label kept the semantic font (prestonbrown/helixscreen#1614).
 *
 * The semantic font must instead ride a shared ADDED style created before the
 * nested bind elements are parsed, so addition order puts a bound font above
 * it. The inline style_text_font attribute stays a local style and keeps
 * outranking both (declarative rule 6).
 *
 * Run with: ./build/bin/helix-tests "[1614]"
 */

#include "../test_fixtures.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_component.h"
#include "lvgl/lvgl.h"
#include "theme_manager.h"

#include "../catch_amalgamated.hpp"

namespace {

constexpr const char* MODE_SUBJECT = "semantic_font_test_mode";

/// One text_small with a compact-font bind, one without, one with an inline
/// font attribute — the three ways a text_* widget's font can be decided.
constexpr const char* FIXTURE_XML = R"(<component>
  <styles>
    <style name="compact_text" text_font="#font_xs"/>
  </styles>
  <view name="root" extends="lv_obj">
    <text_small name="bound_label">
      <bind_style_if_eq name="compact_text" subject="semantic_font_test_mode" ref_value="1"/>
    </text_small>
    <text_small name="plain_label"/>
    <text_small name="inline_label" style_text_font="#font_xs"/>
  </view>
</component>)";

/// The bind's subject. Registered into the global XML scope once; the value is
/// per-test state, so every test sets it before creating the component.
lv_subject_t& mode_subject() {
    static lv_subject_t subject;
    static bool registered = false;
    if (!registered) {
        lv_subject_init_int(&subject, 0);
        lv_xml_register_subject(nullptr, MODE_SUBJECT, &subject);
        registered = true;
    }
    return subject;
}

/// Register the fixture component (idempotent across test cases) and create it.
lv_obj_t* create_fixture(lv_obj_t* parent) {
    static bool registered = false;
    if (!registered) {
        REQUIRE(lv_xml_register_component_from_data("semantic_font_fixture", FIXTURE_XML) ==
                LV_RESULT_OK);
        registered = true;
    }
    return static_cast<lv_obj_t*>(lv_xml_create(parent, "semantic_font_fixture", nullptr));
}

const lv_font_t* resolved_font(const char* token) {
    const lv_font_t* font = theme_manager_get_font(token);
    REQUIRE(font != nullptr);
    return font;
}

} // namespace

TEST_CASE("A bound text_font overrides the semantic font and releases cleanly", "[xml][1614]") {
    XMLTestFixture fixture;
    auto& mode = mode_subject();

    const lv_font_t* small = resolved_font("font_small");
    const lv_font_t* xs = resolved_font("font_xs");
    // The two tiers must resolve to different faces or the assertions below
    // cannot tell the semantic font from the bound one.
    REQUIRE(small != xs);

    lv_obj_t* root = create_fixture(fixture.test_screen());
    REQUIRE(root != nullptr);
    lv_obj_t* bound = lv_obj_find_by_name(root, "bound_label");
    lv_obj_t* plain = lv_obj_find_by_name(root, "plain_label");
    REQUIRE(bound != nullptr);
    REQUIRE(plain != nullptr);

    SECTION("subject selects the bound font, and de-selecting restores the semantic one") {
        lv_subject_set_int(&mode, 1);
        CHECK(lv_obj_get_style_text_font(bound, LV_PART_MAIN) == xs);

        lv_subject_set_int(&mode, 0);
        CHECK(lv_obj_get_style_text_font(bound, LV_PART_MAIN) == small);
    }

    SECTION("with the bind inactive the label keeps its semantic font") {
        lv_subject_set_int(&mode, 0);
        CHECK(lv_obj_get_style_text_font(bound, LV_PART_MAIN) == small);
    }

    SECTION("a label with no font bind renders the semantic font") {
        lv_subject_set_int(&mode, 1);
        CHECK(lv_obj_get_style_text_font(plain, LV_PART_MAIN) == small);
    }
}

TEST_CASE("An inline style_text_font still outranks the semantic and bound fonts", "[xml][1614]") {
    XMLTestFixture fixture;
    auto& mode = mode_subject();

    const lv_font_t* small = resolved_font("font_small");
    const lv_font_t* xs = resolved_font("font_xs");
    REQUIRE(small != xs);

    lv_obj_t* root = create_fixture(fixture.test_screen());
    REQUIRE(root != nullptr);
    lv_obj_t* inline_lbl = lv_obj_find_by_name(root, "inline_label");
    REQUIRE(inline_lbl != nullptr);

    // Local on both sides of the subject: the inline attribute is evaluated
    // once at parse time and must hold regardless of the bound style's state.
    lv_subject_set_int(&mode, 1);
    CHECK(lv_obj_get_style_text_font(inline_lbl, LV_PART_MAIN) == xs);
    lv_subject_set_int(&mode, 0);
    CHECK(lv_obj_get_style_text_font(inline_lbl, LV_PART_MAIN) == xs);

    // And it is genuinely overriding a different semantic font, not echoing it.
    CHECK(lv_obj_get_style_text_font(lv_obj_find_by_name(root, "plain_label"), LV_PART_MAIN) ==
          small);
}
