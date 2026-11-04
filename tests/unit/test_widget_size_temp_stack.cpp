// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_size_temp_stack.cpp
 * @brief temp_stack picks its text and icon fonts from physical height, not
 * rowspan.
 *
 * `on_size_changed` (temp_stack_widget.cpp) promotes to `"sm"` whenever
 * `height_px >= h_tall()` — or whenever the stacked icon-above layout was
 * chosen, which is measured at that same size — and reuses the verdict for the
 * icon font (`mdi_icons_24` vs `mdi_icons_16`). Rowspan gates the layout
 * SHAPE (second test case below); it never picks the font on its own. It
 * restyles three
 * kinds of object, all asserted here as real widget state (applied fonts),
 * since the "xs"/"sm" token itself is only a local variable — there is no
 * member or attribute that stores it:
 *   - the nozzle/bed/chamber heater icon glyphs (`nozzle_icon_glyph`,
 *     `bed_icon_glyph`, `chamber_icon_glyph`) — all three are set, the first
 *     two by direct name lookup and the chamber one through the per-row
 *     loop's "icon component" branch
 *   - every label inside each row's `temp_display` (current + unit; the rows
 *     here use `show_target="false"`, so there is no separator/target label)
 *
 * Each resize pairs pixels with a rowspan that would pick the OPPOSITE size
 * under the old `rowspan >= 2` predicate, so an implementation that still
 * reads rowspan fails here instead of passing by coincidence — same
 * technique as test_widget_size_active_spool.cpp.
 */

#include "ui_fonts.h"
#include "ui_temp_display.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "lvgl/src/misc/lv_event_private.h"
#include "panel_widget_size.h"
#include "src/ui/panel_widgets/temp_stack_widget.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::widget_size;

TEST_CASE_METHOD(LVGLUITestFixture, "temp_stack text and icon fonts follow height, not rowspan",
                 "[widget_size][temp_stack][panel_widget]") {
    // Sanity: the two size tokens this widget switches between must actually
    // resolve to different fonts, or every assertion below would trivially
    // agree regardless of which branch ran.
    REQUIRE(theme_manager_get_font("font_xs") != theme_manager_get_font("font_small"));

    PanelWidgetHarness<TempStackWidget> h(test_screen(), state(), nullptr);

    lv_obj_t* nozzle_row = h.child("temp_stack_nozzle_row");
    lv_obj_t* bed_row = h.child("temp_stack_bed_row");
    REQUIRE(nozzle_row != nullptr);
    REQUIRE(bed_row != nullptr);

    lv_obj_t* nozzle_glyph = h.child("nozzle_icon_glyph");
    lv_obj_t* bed_glyph = h.child("bed_icon_glyph");
    lv_obj_t* chamber_glyph = h.child("chamber_icon_glyph");
    REQUIRE(nozzle_glyph != nullptr);
    REQUIRE(bed_glyph != nullptr);
    REQUIRE(chamber_glyph != nullptr);

    // Row layout is [icon component, temp_display] (panel_widget_temp_stack.xml)
    // — the temp_display instances carry no `name` attribute, so they are
    // reached by position rather than lv_obj_find_by_name().
    lv_obj_t* nozzle_temp_display = lv_obj_get_child(nozzle_row, 1);
    lv_obj_t* bed_temp_display = lv_obj_get_child(bed_row, 1);
    REQUIRE(nozzle_temp_display != nullptr);
    REQUIRE(bed_temp_display != nullptr);

    // show_target="false" -> temp_display's only children are current_label
    // (index 0) and unit_label (index 1); both get restyled.
    lv_obj_t* nozzle_current_label = lv_obj_get_child(nozzle_temp_display, 0);
    lv_obj_t* nozzle_unit_label = lv_obj_get_child(nozzle_temp_display, 1);
    lv_obj_t* bed_current_label = lv_obj_get_child(bed_temp_display, 0);
    lv_obj_t* bed_unit_label = lv_obj_get_child(bed_temp_display, 1);
    REQUIRE(nozzle_current_label != nullptr);
    REQUIRE(nozzle_unit_label != nullptr);
    REQUIRE(bed_current_label != nullptr);
    REQUIRE(bed_unit_label != nullptr);

    auto check_size = [&](const char* font_token, const lv_font_t* icon_font) {
        const lv_font_t* text_font = theme_manager_get_font(font_token);
        CHECK(lv_obj_get_style_text_font(nozzle_glyph, LV_PART_MAIN) == icon_font);
        CHECK(lv_obj_get_style_text_font(bed_glyph, LV_PART_MAIN) == icon_font);
        CHECK(lv_obj_get_style_text_font(chamber_glyph, LV_PART_MAIN) == icon_font);
        CHECK(lv_obj_get_style_text_font(nozzle_current_label, LV_PART_MAIN) == text_font);
        CHECK(lv_obj_get_style_text_font(nozzle_unit_label, LV_PART_MAIN) == text_font);
        CHECK(lv_obj_get_style_text_font(bed_current_label, LV_PART_MAIN) == text_font);
        CHECK(lv_obj_get_style_text_font(bed_unit_label, LV_PART_MAIN) == text_font);
    };

    // Below threshold, rowspan agrees (1): "xs" / mdi_icons_16.
    h.resize(1, 1, 150, h_tall() - 1);
    check_size("font_xs", &mdi_icons_16);

    // At threshold, rowspan agrees (2): "sm" / mdi_icons_24.
    h.resize(1, 2, 150, h_tall());
    check_size("font_small", &mdi_icons_24);

    // Contradicting rowspan: rowspan=1 (old predicate -> "xs") but tall
    // pixels -- the pixel verdict ("sm") must win.
    h.resize(1, 1, 150, 200);
    check_size("font_small", &mdi_icons_24);

    // Contradicting rowspan: rowspan=4 (old predicate -> "sm") but short
    // pixels -- the pixel verdict ("xs") must win.
    h.resize(1, 4, 150, 80);
    check_size("font_xs", &mdi_icons_16);
}

/**
 * The icon-above layout (see stacked_row_layout.h). Asserted as geometry rather
 * than as the flex_flow style alone, because "the icon is above its
 * temperature" is the actual requirement — a flow value that some later
 * alignment or size change quietly defeats would still read green off the
 * style bit.
 */
TEST_CASE_METHOD(LVGLUITestFixture, "temp_stack stacks icons above temps only when it fits",
                 "[widget_size][temp_stack][panel_widget]") {
    PanelWidgetHarness<TempStackWidget> h(test_screen(), state(), nullptr);

    lv_obj_t* nozzle_row = h.child("temp_stack_nozzle_row");
    lv_obj_t* bed_row = h.child("temp_stack_bed_row");
    REQUIRE(nozzle_row != nullptr);
    REQUIRE(bed_row != nullptr);

    // Row layout is [icon component, temp_display] (panel_widget_temp_stack.xml).
    lv_obj_t* nozzle_icon = lv_obj_get_child(nozzle_row, 0);
    lv_obj_t* nozzle_temp = lv_obj_get_child(nozzle_row, 1);
    lv_obj_t* bed_icon = lv_obj_get_child(bed_row, 0);
    lv_obj_t* bed_temp = lv_obj_get_child(bed_row, 1);
    REQUIRE(nozzle_icon != nullptr);
    REQUIRE(bed_icon != nullptr);
    REQUIRE(ui_temp_display_is_valid(nozzle_temp));
    REQUIRE(ui_temp_display_is_valid(bed_temp));

    auto expect_side_by_side = [&](lv_obj_t* icon, lv_obj_t* temp) {
        CHECK(lv_obj_get_style_flex_flow(lv_obj_get_parent(icon), LV_PART_MAIN) ==
              LV_FLEX_FLOW_ROW);
        // Icon ends before the temperature begins, and the two overlap
        // vertically rather than sitting on separate lines.
        CHECK(lv_obj_get_x(icon) + lv_obj_get_width(icon) <= lv_obj_get_x(temp));
        CHECK(lv_obj_get_y(icon) < lv_obj_get_y(temp) + lv_obj_get_height(temp));
        CHECK(lv_obj_get_y(temp) < lv_obj_get_y(icon) + lv_obj_get_height(icon));
    };

    auto expect_stacked = [&](lv_obj_t* icon, lv_obj_t* temp) {
        CHECK(lv_obj_get_style_flex_flow(lv_obj_get_parent(icon), LV_PART_MAIN) ==
              LV_FLEX_FLOW_COLUMN);
        // Icon sits entirely above the temperature, and the two overlap
        // horizontally (both centred) rather than sitting side by side.
        CHECK(lv_obj_get_y(icon) + lv_obj_get_height(icon) <= lv_obj_get_y(temp));
        CHECK(lv_obj_get_x(icon) < lv_obj_get_x(temp) + lv_obj_get_width(temp));
        CHECK(lv_obj_get_x(temp) < lv_obj_get_x(icon) + lv_obj_get_width(icon));
    };

    // A widget left at its default 2x2 tracks is one authored grid cell. It
    // keeps the compact rows however many pixels that cell is worth — this
    // height is far more than the stacked layout needs.
    h.resize(2, 2, 150, 400);
    expect_side_by_side(nozzle_icon, nozzle_temp);
    expect_side_by_side(bed_icon, bed_temp);

    // Taller than a cell, with room: stack, at the larger type.
    h.resize(2, 4, 150, 400);
    expect_stacked(nozzle_icon, nozzle_temp);
    expect_stacked(bed_icon, bed_temp);
    CHECK(lv_obj_get_style_text_font(lv_obj_get_child(nozzle_temp, 0), LV_PART_MAIN) ==
          theme_manager_get_font("font_body"));

    // Three tracks is the first size that qualifies on the span gate.
    h.resize(2, 3, 150, 400);
    expect_stacked(nozzle_icon, nozzle_temp);

    // Taller than a cell but with nowhere to put the second line — a track is
    // only a few tens of pixels on the smallest panels. The compact rows win,
    // and must be fully restored, not left half-converted.
    h.resize(2, 4, 150, 40);
    expect_side_by_side(nozzle_icon, nozzle_temp);
    expect_side_by_side(bed_icon, bed_temp);
}

TEST_CASE_METHOD(LVGLUITestFixture, "TempStackWidget: a row belongs to the tile that holds it",
                 "[temp_stack][panel_widget][from_event]") {
    PanelWidgetHarness<TempStackWidget> a(test_screen(), state(), nullptr);
    PanelWidgetHarness<TempStackWidget> b(test_screen(), state(), nullptr);
    auto owner_of = [](lv_obj_t* row) {
        lv_event_t e{};
        e.current_target = row;
        return panel_widget_from_event<TempStackWidget>(&e);
    };

    CHECK(owner_of(a.child("temp_stack_nozzle_row")) == &a.widget());
    CHECK(owner_of(b.child("temp_stack_bed_row")) == &b.widget());

    a.widget().detach_tile();
    CHECK(owner_of(a.child("temp_stack_nozzle_row")) == nullptr);
}
