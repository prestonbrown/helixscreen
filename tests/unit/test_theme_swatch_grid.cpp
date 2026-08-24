// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// theme_swatch_grid is the theme-editor's preset palette. Unlike the general
// color_swatch_grid it is structured: the first 12 swatches are a dark->light
// surface ramp (screen_bg / card_bg / border / text tokens) and the remaining
// 18 are 6 hue families x 3 tones, laid out so that column == hue family.
// These tests pin that structure — a reordered or dropped swatch breaks the
// contract the theme editor's users navigate by.

#include "../lvgl_test_fixture.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "../catch_amalgamated.hpp"

extern "C" {
#include "helix-xml/src/xml/lv_xml.h"
}

namespace {

constexpr int COLS = 6;
constexpr int SURFACE_COUNT = 12;
constexpr int ACCENT_COUNT = 18;
constexpr int TOTAL = SURFACE_COUNT + ACCENT_COUNT;

/// Hue in degrees [0,360). Undefined (returns -1) for near-neutral colors.
double hue_of(uint32_t rgb) {
    const double r = ((rgb >> 16) & 0xFF) / 255.0;
    const double g = ((rgb >> 8) & 0xFF) / 255.0;
    const double b = (rgb & 0xFF) / 255.0;
    const double mx = std::max({r, g, b});
    const double mn = std::min({r, g, b});
    const double c = mx - mn;
    if (c < 0.04) {
        return -1.0; // neutral: no meaningful hue
    }
    double h = 0.0;
    if (mx == r) {
        h = std::fmod((g - b) / c, 6.0);
    } else if (mx == g) {
        h = (b - r) / c + 2.0;
    } else {
        h = (r - g) / c + 4.0;
    }
    h *= 60.0;
    return h < 0 ? h + 360.0 : h;
}

double relative_luminance(uint32_t rgb) {
    return 0.2126 * ((rgb >> 16) & 0xFF) + 0.7152 * ((rgb >> 8) & 0xFF) + 0.0722 * (rgb & 0xFF);
}

/// Smallest angular distance between two hues, in degrees.
double hue_delta(double a, double b) {
    double d = std::fabs(a - b);
    return d > 180.0 ? 360.0 - d : d;
}

std::vector<uint32_t> swatch_colors(lv_obj_t* grid) {
    std::vector<uint32_t> out;
    const uint32_t n = lv_obj_get_child_count(grid);
    out.reserve(n);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t* child = lv_obj_get_child(grid, static_cast<int32_t>(i));
        lv_color_t bg = lv_obj_get_style_bg_color(child, LV_PART_MAIN);
        out.push_back(lv_color_to_u32(bg) & 0xFFFFFF);
    }
    return out;
}

struct GridFixture : public LVGLTestFixture {
    GridFixture() {
        REQUIRE(lv_xml_register_component_from_file("A:ui_xml/globals.xml") == LV_RESULT_OK);
        REQUIRE(lv_xml_register_component_from_file("A:ui_xml/components/theme_swatch_grid.xml") ==
                LV_RESULT_OK);
    }

    lv_obj_t* create() {
        const char* attrs[] = {"swatch_callback", "theme_swatch_clicked_cb", nullptr};
        auto* grid =
            static_cast<lv_obj_t*>(lv_xml_create(lv_screen_active(), "theme_swatch_grid", attrs));
        REQUIRE(grid != nullptr);
        return grid;
    }
};

} // namespace

TEST_CASE_METHOD(GridFixture, "theme_swatch_grid: exposes exactly 30 swatches in 6 columns",
                 "[xml][theme][swatch]") {
    lv_obj_t* grid = create();
    REQUIRE(lv_obj_get_child_count(grid) == TOTAL);
    REQUIRE(TOTAL % COLS == 0);
}

TEST_CASE_METHOD(GridFixture, "theme_swatch_grid: every swatch is clickable",
                 "[xml][theme][swatch]") {
    lv_obj_t* grid = create();
    for (uint32_t i = 0; i < lv_obj_get_child_count(grid); i++) {
        lv_obj_t* child = lv_obj_get_child(grid, static_cast<int32_t>(i));
        INFO("swatch index " << i);
        REQUIRE(lv_obj_has_flag(child, LV_OBJ_FLAG_CLICKABLE));
    }
}

TEST_CASE_METHOD(GridFixture, "theme_swatch_grid: surface block is a monotonic dark->light ramp",
                 "[xml][theme][swatch]") {
    lv_obj_t* grid = create();
    auto colors = swatch_colors(grid);
    REQUIRE(colors.size() == TOTAL);

    // The theme editor's surface tokens (screen_bg -> text) are picked by
    // lightness, so the first 12 swatches must read as an ordered ramp.
    for (int i = 1; i < SURFACE_COUNT; i++) {
        const double prev = relative_luminance(colors[i - 1]);
        const double cur = relative_luminance(colors[i]);
        INFO("surface swatch " << i << " luminance " << cur << " must exceed " << prev);
        REQUIRE(cur > prev);
    }

    // The ramp must actually span the usable range: dark enough for an OLED-black
    // screen_bg, light enough for a light-mode background.
    REQUIRE(relative_luminance(colors.front()) < 30.0);
    REQUIRE(relative_luminance(colors[SURFACE_COUNT - 1]) > 230.0);
}

TEST_CASE_METHOD(GridFixture, "theme_swatch_grid: accent columns hold one hue family each",
                 "[xml][theme][swatch]") {
    lv_obj_t* grid = create();
    auto colors = swatch_colors(grid);

    // red, amber, green, teal, blue, violet — the six families the accent
    // block is organised by. Column position is the navigation affordance.
    const double family_hue[COLS] = {0.0, 42.0, 110.0, 175.0, 215.0, 275.0};
    constexpr double TOLERANCE = 32.0;

    for (int row = 0; row < ACCENT_COUNT / COLS; row++) {
        for (int col = 0; col < COLS; col++) {
            const uint32_t rgb = colors[SURFACE_COUNT + row * COLS + col];
            const double h = hue_of(rgb);
            INFO("accent row " << row << " col " << col << " rgb=" << std::hex << rgb
                               << " hue=" << std::dec << h);
            REQUIRE(h >= 0.0); // accents must be chromatic, not neutral
            REQUIRE(hue_delta(h, family_hue[col]) <= TOLERANCE);
        }
    }
}

TEST_CASE_METHOD(GridFixture, "theme_swatch_grid: accent rows darken from muted to deep",
                 "[xml][theme][swatch]") {
    lv_obj_t* grid = create();
    auto colors = swatch_colors(grid);

    // Row 0 = muted (dark-mode accents), row 2 = deep (light-mode accents).
    // Each column must get consistently darker down the block, otherwise the
    // "pick a row for your mode" affordance is a lie.
    for (int col = 0; col < COLS; col++) {
        const double muted = relative_luminance(colors[SURFACE_COUNT + 0 * COLS + col]);
        const double mid = relative_luminance(colors[SURFACE_COUNT + 1 * COLS + col]);
        const double deep = relative_luminance(colors[SURFACE_COUNT + 2 * COLS + col]);
        INFO("accent column " << col << " luminance " << muted << " / " << mid << " / " << deep);
        REQUIRE(muted > mid);
        REQUIRE(mid > deep);
    }
}

// --- palette selection -------------------------------------------------
// color_picker.xml switches grids with <if cond="color_picker_palette eq 1">.
// This mirrors that block verbatim (same cond, same two component names) so a
// typo'd component name or a broken cond fails here rather than at runtime.

namespace {

constexpr uint32_t THEME_FIRST_SWATCH = 0x0B0D14;   // theme_swatch_grid row 1 col 1
constexpr uint32_t GENERAL_FIRST_SWATCH = 0x1A1A1A; // color_swatch_grid row 1 col 1

constexpr const char* PALETTE_SWITCH_COMPONENT =
    "<component><view><lv_obj name='host'>"
    "  <if cond='color_picker_palette eq 1'>"
    "    <theme_swatch_grid swatch_callback='color_swatch_clicked_cb'/>"
    "    <else/>"
    "    <color_swatch_grid swatch_callback='color_swatch_clicked_cb'/>"
    "  </if>"
    "</lv_obj></view></component>";

struct PaletteSwitchFixture : public LVGLTestFixture {
    lv_subject_t palette_subject{};

    PaletteSwitchFixture() {
        REQUIRE(lv_xml_register_component_from_file("A:ui_xml/globals.xml") == LV_RESULT_OK);
        REQUIRE(lv_xml_register_component_from_file("A:ui_xml/components/color_swatch_grid.xml") ==
                LV_RESULT_OK);
        REQUIRE(lv_xml_register_component_from_file("A:ui_xml/components/theme_swatch_grid.xml") ==
                LV_RESULT_OK);
        lv_subject_init_int(&palette_subject, 0);
        lv_xml_register_subject(nullptr, "color_picker_palette", &palette_subject);
        REQUIRE(lv_xml_register_component_from_data("palette_switch_probe",
                                                    PALETTE_SWITCH_COMPONENT) == LV_RESULT_OK);
    }

    ~PaletteSwitchFixture() {
        lv_xml_component_unregister("palette_switch_probe");
        lv_subject_deinit(&palette_subject);
    }

    /// First swatch color of whichever grid the <if> built.
    uint32_t built_grid_first_swatch() {
        auto* view = static_cast<lv_obj_t*>(
            lv_xml_create(lv_screen_active(), "palette_switch_probe", nullptr));
        REQUIRE(view != nullptr);
        lv_obj_t* host = lv_obj_find_by_name(view, "host");
        REQUIRE(host != nullptr);
        REQUIRE(lv_obj_get_child_count(host) == 1); // exactly one grid, never both
        lv_obj_t* grid = lv_obj_get_child(host, 0);
        REQUIRE(lv_obj_get_child_count(grid) == TOTAL);
        lv_color_t bg = lv_obj_get_style_bg_color(lv_obj_get_child(grid, 0), LV_PART_MAIN);
        return lv_color_to_u32(bg) & 0xFFFFFF;
    }
};

} // namespace

TEST_CASE_METHOD(PaletteSwitchFixture, "color picker palette: Theme selects the theme grid",
                 "[xml][theme][swatch]") {
    lv_subject_set_int(&palette_subject, 1); // ColorPicker::Palette::Theme
    REQUIRE(built_grid_first_swatch() == THEME_FIRST_SWATCH);
}

TEST_CASE_METHOD(PaletteSwitchFixture, "color picker palette: General selects the shared grid",
                 "[xml][theme][swatch]") {
    lv_subject_set_int(&palette_subject, 0); // ColorPicker::Palette::General
    REQUIRE(built_grid_first_swatch() == GENERAL_FIRST_SWATCH);
}

TEST_CASE_METHOD(GridFixture, "theme_swatch_grid: no two swatches are visually identical",
                 "[xml][theme][swatch]") {
    lv_obj_t* grid = create();
    auto colors = swatch_colors(grid);
    for (size_t i = 0; i < colors.size(); i++) {
        for (size_t j = i + 1; j < colors.size(); j++) {
            INFO("swatch " << i << " vs " << j << " both " << std::hex << colors[i]);
            REQUIRE(colors[i] != colors[j]);
        }
    }
}
