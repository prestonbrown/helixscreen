// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// A theme switch and the palette walker against colors the XML author wrote
// inline. The engine records every inline `style_*` color written as a global
// `#const` or a literal; the switch re-resolves the `#const` ones, and the
// walker leaves every authored color alone while still theming the rest.

#include "../test_fixtures.h"
#include "core/subject.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_component.h"
#include "lvgl/lvgl.h"
#include "theme_loader.h"
#include "theme_manager.h"

#include <cmath>
#include <utility>

#include "../catch_amalgamated.hpp"

namespace {

/// Puts the fixture's theme and mode back so later cases see what they expect.
struct RestoreTheme {
    helix::ThemeData theme = theme_manager_get_active_theme();
    bool dark = theme_manager_is_dark_mode();
    ~RestoreTheme() {
        theme_manager_apply_theme(theme, dark);
    }
};

uint32_t rgb(lv_color_t c) {
    return lv_color_to_u32(c) & 0xFFFFFF;
}

uint32_t hex(const std::string& s) {
    return rgb(theme_manager_parse_hex_color(s.c_str()));
}

uint32_t const_rgb(const char* name) {
    const char* v = lv_xml_get_const_silent(nullptr, name);
    REQUIRE(v != nullptr);
    return rgb(theme_manager_parse_hex_color(v));
}

uint32_t text_rgb(lv_obj_t* obj) {
    return rgb(lv_obj_get_style_text_color(obj, LV_PART_MAIN));
}

uint32_t bg_rgb(lv_obj_t* obj) {
    return rgb(lv_obj_get_style_bg_color(obj, LV_PART_MAIN));
}

lv_obj_t* named(lv_obj_t* root, const char* name) {
    lv_obj_t* obj = lv_obj_find_by_name(root, name);
    REQUIRE(obj != nullptr);
    return obj;
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "the palette walker keeps an inline token color on a plain label",
                 "[theme][xml]") {
    const char* xml = R"(<component>
  <view extends="lv_obj" width="300" height="300">
    <lv_label name="tok" text="Primary" style_text_color="#primary"/>
  </view>
</component>)";
    REQUIRE(lv_xml_register_component_from_data("lr_plain_token", xml) == LV_RESULT_OK);
    lv_obj_t* root = create_component("lr_plain_token");
    REQUIRE(root != nullptr);
    lv_obj_t* tok = named(root, "tok");

    const uint32_t primary = const_rgb("primary");
    REQUIRE(primary != const_rgb("text"));
    REQUIRE(text_rgb(tok) == primary);

    theme_apply_current_palette_to_tree(root);
    CHECK(text_rgb(tok) == primary);
}

TEST_CASE_METHOD(XMLTestFixture, "the palette walker still themes a label with no inline color",
                 "[theme][xml]") {
    const char* xml = R"(<component>
  <view extends="lv_obj" width="300" height="300">
    <lv_label name="plain" text="Plain"/>
  </view>
</component>)";
    REQUIRE(lv_xml_register_component_from_data("lr_plain_label", xml) == LV_RESULT_OK);
    lv_obj_t* root = create_component("lr_plain_label");
    REQUIRE(root != nullptr);
    lv_obj_t* plain = named(root, "plain");

    lv_obj_set_style_text_color(plain, lv_color_hex(0x00FF00), LV_PART_MAIN);
    theme_apply_current_palette_to_tree(root);
    CHECK(text_rgb(plain) == const_rgb("text"));
}

TEST_CASE_METHOD(XMLTestFixture,
                 "a tree built before a mode switch shows the new mode's token colors after it",
                 "[theme][xml]") {
    RestoreTheme restore;
    const auto theme = helix::get_builtin_fallback_theme();
    theme_manager_apply_theme(theme, false);
    REQUIRE(hex(theme.light.card_bg) != hex(theme.dark.card_bg));
    REQUIRE(hex(theme.light.text_muted) != hex(theme.dark.text_muted));

    const char* xml = R"(<component>
  <view extends="lv_obj" width="300" height="300">
    <lv_obj name="card" width="200" height="100" style_bg_opa="255" style_bg_color="#card_bg">
      <text_small name="muted" text="Muted" style_text_color="#text_muted"/>
      <lv_label name="plain_muted" text="Muted" style_text_color="#text_muted"/>
    </lv_obj>
  </view>
</component>)";
    REQUIRE(lv_xml_register_component_from_data("lr_mode_switch", xml) == LV_RESULT_OK);
    lv_obj_t* root = create_component("lr_mode_switch");
    REQUIRE(root != nullptr);
    // A popup on the top layer is outside the active screen's tree.
    lv_obj_t* top =
        static_cast<lv_obj_t*>(lv_xml_create(lv_layer_top(), "lr_mode_switch", nullptr));
    REQUIRE(top != nullptr);

    CHECK(bg_rgb(named(root, "card")) == hex(theme.light.card_bg));
    CHECK(text_rgb(named(root, "muted")) == hex(theme.light.text_muted));

    theme_manager_apply_theme(theme, true);
    CHECK(bg_rgb(named(root, "card")) == hex(theme.dark.card_bg));
    CHECK(text_rgb(named(root, "muted")) == hex(theme.dark.text_muted));
    CHECK(text_rgb(named(root, "plain_muted")) == hex(theme.dark.text_muted));
    CHECK(bg_rgb(named(top, "card")) == hex(theme.dark.card_bg));
    CHECK(text_rgb(named(top, "muted")) == hex(theme.dark.text_muted));

    theme_manager_apply_theme(theme, false);
    CHECK(bg_rgb(named(root, "card")) == hex(theme.light.card_bg));
    CHECK(text_rgb(named(root, "muted")) == hex(theme.light.text_muted));

    lv_obj_delete(top);
}

TEST_CASE_METHOD(XMLTestFixture, "a token color C++ overwrote after creation survives a switch",
                 "[theme][xml]") {
    RestoreTheme restore;
    const auto theme = helix::get_builtin_fallback_theme();
    theme_manager_apply_theme(theme, false);

    const char* xml = R"(<component>
  <view extends="lv_obj" width="300" height="300">
    <text_small name="status" text="Status" style_text_color="#text_muted"/>
  </view>
</component>)";
    REQUIRE(lv_xml_register_component_from_data("lr_cpp_overwrite", xml) == LV_RESULT_OK);
    lv_obj_t* root = create_component("lr_cpp_overwrite");
    REQUIRE(root != nullptr);
    lv_obj_t* status = named(root, "status");

    lv_obj_set_style_text_color(status, lv_color_hex(0xC01020), LV_PART_MAIN);
    theme_manager_apply_theme(theme, true);
    CHECK(text_rgb(status) == 0xC01020);
}

TEST_CASE_METHOD(XMLTestFixture, "a literal white status text survives a mode switch",
                 "[theme][xml]") {
    RestoreTheme restore;
    const auto theme = helix::get_builtin_fallback_theme();
    theme_manager_apply_theme(theme, true);

    // The QR scanner's white status line. Kept off a dark ancestor here, where
    // the walker's light-text rule would paint white anyway and prove nothing.
    const char* xml = R"(<component>
  <view extends="lv_obj" width="300" height="300">
    <lv_obj name="backdrop" width="200" height="100" style_bg_opa="255" style_bg_color="0x000000"/>
    <text_body name="status" text="Scanning" style_text_color="#FFFFFF"/>
  </view>
</component>)";
    REQUIRE(lv_xml_register_component_from_data("lr_literal_white", xml) == LV_RESULT_OK);
    lv_obj_t* root = create_component("lr_literal_white");
    REQUIRE(root != nullptr);

    theme_manager_apply_theme(theme, false);
    CHECK(text_rgb(named(root, "status")) == 0xFFFFFF);
    CHECK(bg_rgb(named(root, "backdrop")) == 0x000000);
    theme_manager_apply_theme(theme, true);
    CHECK(text_rgb(named(root, "status")) == 0xFFFFFF);
}

namespace {

/// An int subject registered under `name` for the life of one test.
struct ScopedIntSubject {
    helix::Subject subject{};
    explicit ScopedIntSubject(const char* name, int32_t value) {
        subject.init_int(value);
        lv_xml_register_subject(nullptr, name, &subject);
    }
    ~ScopedIntSubject() {
        subject.reset();
    }
};

double luminance(uint32_t rgb_hex) {
    auto ch = [](uint32_t c) {
        double v = c / 255.0;
        return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * ch((rgb_hex >> 16) & 0xFF) + 0.7152 * ch((rgb_hex >> 8) & 0xFF) +
           0.0722 * ch(rgb_hex & 0xFF);
}

double contrast(uint32_t a, uint32_t b) {
    double la = luminance(a), lb = luminance(b);
    if (la < lb)
        std::swap(la, lb);
    return (la + 0.05) / (lb + 0.05);
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "a component style's token color follows a mode switch",
                 "[theme][xml]") {
    RestoreTheme restore;
    const auto theme = helix::get_builtin_fallback_theme();
    theme_manager_apply_theme(theme, true);
    REQUIRE(hex(theme.dark.elevated_bg) != hex(theme.light.elevated_bg));

    // The bed mesh profiles card's active row: a named style, not an inline color.
    const char* xml = R"(<component>
  <styles>
    <style name="lr_active_row" bg_opa="255" bg_color="#elevated_bg"/>
  </styles>
  <view extends="lv_obj" width="300" height="300">
    <lv_obj name="row" width="200" height="40"><style name="lr_active_row"/></lv_obj>
  </view>
</component>)";
    REQUIRE(lv_xml_register_component_from_data("lr_named_style", xml) == LV_RESULT_OK);
    lv_obj_t* root = create_component("lr_named_style");
    REQUIRE(root != nullptr);
    REQUIRE(bg_rgb(named(root, "row")) == hex(theme.dark.elevated_bg));

    theme_manager_apply_theme(theme, false);
    // A row built after the switch takes the style as it is now.
    lv_obj_t* later = create_component("lr_named_style");
    REQUIRE(later != nullptr);
    CHECK(bg_rgb(named(later, "row")) == hex(theme.light.elevated_bg));
    CHECK(bg_rgb(named(root, "row")) == hex(theme.light.elevated_bg));
}

TEST_CASE_METHOD(XMLTestFixture,
                 "a label's text color from a bound or component style beats the walker",
                 "[theme][xml]") {
    ScopedIntSubject sel("lr_bound_sel", 1);
    const char* xml = R"(<component>
  <styles>
    <style name="lr_accent_text" text_color="#primary"/>
  </styles>
  <view extends="lv_obj" width="300" height="300">
    <lv_label name="bound" text="Bound">
      <bind_style_if_eq name="lr_accent_text" subject="lr_bound_sel" ref_value="1"/>
    </lv_label>
    <lv_label name="styled" text="Styled"><style name="lr_accent_text"/></lv_label>
    <lv_obj name="dark" width="200" height="100" style_bg_opa="255" style_bg_color="0x000000">
      <text_body name="bound_on_dark" text="Bound">
        <bind_style_if_eq name="lr_accent_text" subject="lr_bound_sel" ref_value="1"/>
      </text_body>
      <text_body name="semantic_on_dark" text="Plain"/>
    </lv_obj>
  </view>
</component>)";
    REQUIRE(lv_xml_register_component_from_data("lr_bound_text", xml) == LV_RESULT_OK);
    lv_obj_t* root = create_component("lr_bound_text");
    REQUIRE(root != nullptr);
    const uint32_t primary = const_rgb("primary");
    REQUIRE(primary != const_rgb("text"));
    REQUIRE(primary != 0xFFFFFF);

    theme_apply_current_palette_to_tree(root);
    CHECK(text_rgb(named(root, "bound")) == primary);
    CHECK(text_rgb(named(root, "styled")) == primary);
    CHECK(text_rgb(named(root, "bound_on_dark")) == primary);
    // Only the shared semantic text style: the dark-ancestor rule still applies.
    CHECK(text_rgb(named(root, "semantic_on_dark")) == 0xFFFFFF);
}

// lv_obj_bind_style attaches the style once and toggles it disabled, so a
// binding that is off when the walker runs must still keep the walker off.
TEST_CASE_METHOD(XMLTestFixture, "a bound text style off at a switch shows when it turns on later",
                 "[theme][xml][motion]") {
    RestoreTheme restore;
    const auto theme = helix::get_builtin_fallback_theme();
    theme_manager_apply_theme(theme, true);
    ScopedIntSubject active("lr_late_active", 1);
    helix::Subject label_subject{};
    static char label_buf[32];
    label_subject.init_string(label_buf, nullptr, sizeof(label_buf), "Move");
    lv_xml_register_subject(nullptr, "lr_late_label", &label_subject);
    REQUIRE(lv_xml_register_component_from_file("A:ui_xml/components/zone_tab.xml") ==
            LV_RESULT_OK);

    // Selected at the switch, so the idle label style is attached but disabled.
    const char* xml = R"(<component>
  <view extends="lv_obj" width="400" height="200">
    <zone_tab name="tab" label_subject="lr_late_label" active_subject="lr_late_active" tab_index="0"
              callback="on_motion_tab_clicked" label_idle_style="zone_tab_label_muted"/>
  </view>
</component>)";
    REQUIRE(lv_xml_register_component_from_data("lr_late_bound", xml) == LV_RESULT_OK);
    lv_obj_t* root = create_component("lr_late_bound");
    REQUIRE(root != nullptr);
    lv_obj_t* label = named(named(root, "tab"), "tab_label");

    theme_manager_apply_theme(theme, false);
    lv_subject_set_int(&active.subject, 0);
    CHECK(text_rgb(label) == hex(theme.light.text_muted));

    label_subject.reset();
}

// A status color handed down through a component prop (history_list_row's
// row_status) is the author's, whichever token or literal the caller passed.
TEST_CASE_METHOD(XMLTestFixture, "a text color passed through a component prop survives a switch",
                 "[theme][xml]") {
    RestoreTheme restore;
    const auto theme = helix::get_builtin_fallback_theme();
    theme_manager_apply_theme(theme, true);

    REQUIRE(lv_xml_register_component_from_data("lr_status_row", R"(<component>
  <api><prop name="status_color" type="color" default="0x888888"/></api>
  <view extends="lv_obj" width="300" height="60">
    <text_small name="row_status" text="Completed" style_text_color="$status_color"/>
  </view>
</component>)") == LV_RESULT_OK);
    REQUIRE(lv_xml_register_component_from_data("lr_status_list", R"(<component>
  <view extends="lv_obj" width="300" height="200" flex_flow="column">
    <lr_status_row name="done" status_color="#success"/>
    <lr_status_row name="failed" status_color="0xD03030"/>
  </view>
</component>)") == LV_RESULT_OK);
    lv_obj_t* root = create_component("lr_status_list");
    REQUIRE(root != nullptr);
    lv_obj_t* done = named(named(root, "done"), "row_status");
    lv_obj_t* failed = named(named(root, "failed"), "row_status");
    const uint32_t success = const_rgb("success");
    REQUIRE(text_rgb(done) == success);
    REQUIRE(success != hex(theme.light.text_muted));

    theme_manager_apply_theme(theme, false);
    CHECK(text_rgb(done) == success);
    CHECK(text_rgb(failed) == 0xD03030);
}

TEST_CASE_METHOD(XMLTestFixture,
                 "a selected motion rail tab shows the new mode's colors after a switch",
                 "[theme][xml][motion]") {
    RestoreTheme restore;
    const auto theme = helix::get_builtin_fallback_theme();
    theme_manager_apply_theme(theme, true);
    ScopedIntSubject active("lr_tab_active", 1);
    ScopedIntSubject idle("lr_tab_idle", 0);
    helix::Subject label_subject{};
    static char label_buf[32];
    label_subject.init_string(label_buf, nullptr, sizeof(label_buf), "Move");
    lv_xml_register_subject(nullptr, "lr_tab_label", &label_subject);

    REQUIRE(lv_xml_register_component_from_file("A:ui_xml/components/zone_tab.xml") ==
            LV_RESULT_OK);
    // The motion panel's rail tabs, as ui_xml/motion_panel.xml instantiates them.
    const char* xml = R"(<component>
  <view extends="lv_obj" width="400" height="200">
    <zone_tab name="sel_tab" label_subject="lr_tab_label" active_subject="lr_tab_active" tab_index="0"
              icon="cursor_move" callback="on_motion_tab_clicked"
              selected_style="zone_tab_pill_selected" idle_style="zone_tab_pill_idle"
              content_selected_style="zone_tab_on_primary"/>
    <zone_tab name="idle_tab" label_subject="lr_tab_label" active_subject="lr_tab_idle" tab_index="1"
              icon="crosshairs_gps" callback="on_motion_tab_clicked"
              selected_style="zone_tab_pill_selected" idle_style="zone_tab_pill_idle"
              content_selected_style="zone_tab_on_primary"/>
  </view>
</component>)";
    REQUIRE(lv_xml_register_component_from_data("lr_motion_tabs", xml) == LV_RESULT_OK);
    lv_obj_t* root = create_component("lr_motion_tabs");
    REQUIRE(root != nullptr);
    lv_obj_t* sel_tab = named(root, "sel_tab");
    lv_obj_t* sel_label = named(sel_tab, "tab_label");
    REQUIRE(bg_rgb(sel_tab) == hex(theme.dark.primary));

    theme_manager_apply_theme(theme, false);
    // The selected pill's label is bound to zone_tab_on_primary, so it takes
    // the new mode's text_on_primary.
    const char* on_primary = lv_xml_get_const(nullptr, "text_on_primary");
    REQUIRE(on_primary != nullptr);
    const uint32_t want_text = rgb(theme_manager_parse_hex_color(on_primary));
    CHECK(bg_rgb(sel_tab) == hex(theme.light.primary));
    CHECK(text_rgb(sel_label) == want_text);
    INFO("light contrast label/pill = " << contrast(text_rgb(sel_label), bg_rgb(sel_tab)));
    CHECK(contrast(text_rgb(sel_label), bg_rgb(sel_tab)) >= 4.0);

    // A tab built after the switch takes the pill style as it is now.
    lv_obj_t* fresh = create_component("lr_motion_tabs");
    REQUIRE(fresh != nullptr);
    lv_obj_t* fresh_tab = named(fresh, "sel_tab");
    CHECK(bg_rgb(fresh_tab) == hex(theme.light.primary));
    lv_obj_t* fresh_label = named(fresh_tab, "tab_label");
    INFO(
        "fresh light contrast label/pill = " << contrast(text_rgb(fresh_label), bg_rgb(fresh_tab)));
    CHECK(contrast(text_rgb(fresh_label), bg_rgb(fresh_tab)) >= 4.0);

    label_subject.reset();
}

TEST_CASE_METHOD(XMLTestFixture,
                 "an idle motion rail pill keeps its style's card fill after a switch",
                 "[theme][xml][motion]") {
    RestoreTheme restore;
    const auto theme = helix::get_builtin_fallback_theme();
    theme_manager_apply_theme(theme, true);
    ScopedIntSubject idle("lr_pill_idle", 0);
    helix::Subject label_subject{};
    static char label_buf[32];
    label_subject.init_string(label_buf, nullptr, sizeof(label_buf), "Bed");
    lv_xml_register_subject(nullptr, "lr_pill_label", &label_subject);
    REQUIRE(lv_xml_register_component_from_file("A:ui_xml/components/zone_tab.xml") ==
            LV_RESULT_OK);

    const char* xml = R"(<component>
  <view extends="lv_obj" width="400" height="200">
    <zone_tab name="idle_tab" label_subject="lr_pill_label" active_subject="lr_pill_idle" tab_index="1"
              icon="crosshairs_gps" callback="on_motion_tab_clicked"
              selected_style="zone_tab_pill_selected" idle_style="zone_tab_pill_idle"
              content_selected_style="zone_tab_on_primary"/>
  </view>
</component>)";
    REQUIRE(lv_xml_register_component_from_data("lr_idle_pill", xml) == LV_RESULT_OK);
    lv_obj_t* root = create_component("lr_idle_pill");
    REQUIRE(root != nullptr);
    lv_obj_t* tab = named(root, "idle_tab");
    REQUIRE(bg_rgb(tab) == hex(theme.dark.card_bg));
    REQUIRE(hex(theme.light.card_bg) != hex(theme.light.elevated_bg));

    theme_manager_apply_theme(theme, false);
    CHECK(bg_rgb(tab) == hex(theme.light.card_bg));

    label_subject.reset();
}

TEST_CASE_METHOD(XMLTestFixture, "the palette walker still themes a neutral button's fill",
                 "[theme][xml]") {
    const char* xml = R"(<component>
  <view extends="lv_obj" width="300" height="300">
    <ui_button name="btn" variant="transparent" text="Cancel"/>
  </view>
</component>)";
    REQUIRE(lv_xml_register_component_from_data("lr_plain_button", xml) == LV_RESULT_OK);
    lv_obj_t* root = create_component("lr_plain_button");
    REQUIRE(root != nullptr);
    lv_obj_t* btn = named(root, "btn");

    // ThemeManager's shared variant style is not a chosen style: the walker
    // still gives this gray button the palette's control surface.
    const uint32_t elevated = const_rgb("elevated_bg");
    REQUIRE(bg_rgb(btn) != elevated);
    theme_apply_current_palette_to_tree(root);
    CHECK(bg_rgb(btn) == elevated);
}

TEST_CASE_METHOD(XMLTestFixture, "chart_measured is each theme's primary, readable on the plot",
                 "[theme][chart_measured]") {
    RestoreTheme restore;
    const std::string dir = helix::get_default_themes_directory();
    const auto themes = helix::discover_themes(dir);
    REQUIRE(themes.size() > 1);
    for (const auto& info : themes) {
        const helix::ThemeData theme =
            helix::load_theme_from_file(dir + "/" + info.filename + ".json");
        REQUIRE(theme.is_valid());
        for (const bool dark : {true, false}) {
            if (dark ? !theme.supports_dark() : !theme.supports_light()) {
                continue;
            }
            theme_manager_apply_theme(theme, dark);
            const helix::ModePalette& palette = dark ? theme.dark : theme.light;
            const uint32_t measured = const_rgb("chart_measured");
            const uint32_t plot = hex(palette.screen_bg);
            const uint32_t primary = hex(palette.primary);
            INFO(info.filename << (dark ? " dark" : " light") << ": chart_measured=" << std::hex
                               << measured << " primary=" << primary << " plot=" << plot);
            // The curve sits on the chart's screen_bg plot.
            CHECK(contrast(measured, plot) >= helix::kChartCurveContrastThreshold);
            if (contrast(primary, plot) >= helix::kChartCurveContrastThreshold) {
                CHECK(measured == primary);
            }
            CHECK(rgb(theme_manager_get_color("chart_measured")) == measured);
        }
    }
}
