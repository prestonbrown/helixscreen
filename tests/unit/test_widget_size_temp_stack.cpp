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

#include "ui_carousel.h"
#include "ui_fonts.h"
#include "ui_temp_display.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/scoped_breakpoint.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/src/misc/lv_event_private.h"
#include "panel_widget_size.h"
#include "src/ui/panel_widgets/temp_stack_widget.h"

#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::widget_size;

TEST_CASE_METHOD(LVGLUITestFixture, "temp_stack text and icon fonts follow height, not rowspan",
                 "[widget_size][temp_stack][panel_widget]") {
    // Sanity: the two size tokens this widget switches between must actually
    // resolve to different fonts, or every assertion below would trivially
    // agree regardless of which branch ran.
    REQUIRE(theme_manager_get_font("font_xs") != theme_manager_get_font("font_small"));

    PanelWidgetHarness<TempStackWidget> h(test_screen(), "temp_stack", state(), nullptr);

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
    PanelWidgetHarness<TempStackWidget> h(test_screen(), "temp_stack", state(), nullptr);

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
    PanelWidgetHarness<TempStackWidget> a(test_screen(), "temp_stack_a", state(), nullptr);
    PanelWidgetHarness<TempStackWidget> b(test_screen(), "temp_stack_b", state(), nullptr);
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

// Carousel mode builds one page per heater: nozzle and bed always, each a
// clickable page named for the heater it opens, holding that heater's icon and
// a temp_display whose children pass taps through to the page.
TEST_CASE_METHOD(LVGLUITestFixture, "temp_stack carousel pages open their own heater",
                 "[temp_stack][panel_widget][carousel]") {
    PanelWidgetHarness<TempStackWidget> h(test_screen(),
                                          HarnessConfig{{{"display_mode", "carousel"}}},
                                          "temp_stack", state(), nullptr);

    lv_obj_t* nozzle = h.child("nozzle");
    lv_obj_t* bed = h.child("bed");
    REQUIRE(nozzle != nullptr);
    REQUIRE(bed != nullptr);
    for (lv_obj_t* page : {nozzle, bed}) {
        CHECK(lv_obj_is_clickable(page));
        REQUIRE(lv_obj_get_child_count(page) == 2);
        lv_obj_t* display = lv_obj_get_child(page, 1);
        CHECK(lv_obj_check_type(display, &lv_obj_class));
        CHECK_FALSE(lv_obj_is_clickable(display));
        CHECK(lv_obj_is_event_bubble(display));
    }
    CHECK(lv_obj_find_by_name(nozzle, "carousel_nozzle_icon") != nullptr);
    CHECK(lv_obj_find_by_name(bed, "bed_icon_glyph") != nullptr);

    // A page's temp_display shows the heater's target beside its reading.
    lv_obj_t* bed_display = lv_obj_get_child(bed, 1);
    CHECK(lv_obj_get_child_count(bed_display) > 2);
}

// The chamber page joins the carousel only on a printer with a chamber.
TEST_CASE_METHOD(LVGLUITestFixture, "temp_stack carousel shows a chamber page only with a chamber",
                 "[temp_stack][panel_widget][carousel]") {
    lv_subject_t* has_chamber = lv_xml_get_subject(nullptr, "printer_has_chamber");
    REQUIRE(has_chamber != nullptr);
    const bool chamber = GENERATE(false, true);
    lv_subject_set_int(has_chamber, chamber ? 1 : 0);
    PanelWidgetHarness<TempStackWidget> h(test_screen(),
                                          HarnessConfig{{{"display_mode", "carousel"}}},
                                          "temp_stack", state(), nullptr);

    lv_obj_t* carousel = h.child("temp_carousel");
    CarouselState* cs = ui_carousel_get_state(carousel);
    REQUIRE(cs != nullptr);
    int reachable = 0;
    for (lv_obj_t* tile : cs->real_tiles) {
        reachable += lv_obj_is_hidden(tile) ? 0 : 1;
    }
    INFO("chamber=" << chamber);
    CHECK(reachable == (chamber ? 3 : 2));
    lv_obj_t* page = h.child("chamber");
    const bool page_shown = page && !lv_obj_is_hidden(lv_obj_get_parent(page));
    CHECK(page_shown == chamber);
    if (chamber) {
        CHECK(lv_obj_find_by_name(page, "chamber_icon_glyph") != nullptr);
    }
    lv_subject_set_int(has_chamber, 0);
}

// A box wider than a cell lays the heaters across it, one column each, instead
// of leaving the width empty (prestonbrown/helixscreen#2198).
TEST_CASE_METHOD(LVGLUITestFixture, "temp_stack at 2x1 lays the readouts side by side",
                 "[widget_size][temp_stack][panel_widget][wide]") {
    PanelWidgetHarness<TempStackWidget> h(test_screen(), "temp_stack", state(), nullptr);

    lv_obj_t* nozzle_row = h.child("temp_stack_nozzle_row");
    lv_obj_t* bed_row = h.child("temp_stack_bed_row");
    lv_obj_t* nozzle_label = h.child("temp_stack_nozzle_label");
    lv_obj_t* bed_label = h.child("temp_stack_bed_label");
    REQUIRE(nozzle_row != nullptr);
    REQUIRE(bed_row != nullptr);
    REQUIRE(nozzle_label != nullptr);
    REQUIRE(bed_label != nullptr);
    lv_obj_t* nozzle_temp = lv_obj_get_child(nozzle_row, 1);
    lv_obj_t* bed_temp = lv_obj_get_child(bed_row, 1);
    REQUIRE(ui_temp_display_is_valid(nozzle_temp));

    const int box_w = 2 * w_normal();
    const int box_h = 4 * h_tall() / 3;

    // 2x1 cells = 4x2 tracks.
    h.resize(4, 2, box_w, box_h);
    lv_obj_update_layout(h.root());

    // One row of columns: same top, left to right, each inside the box.
    CHECK(lv_obj_get_style_flex_flow(h.root(), LV_PART_MAIN) == LV_FLEX_FLOW_ROW);
    CHECK(lv_obj_get_x(nozzle_row) + lv_obj_get_width(nozzle_row) <= lv_obj_get_x(bed_row));
    CHECK(lv_obj_get_y(nozzle_row) == lv_obj_get_y(bed_row));
    CHECK(lv_obj_get_x(bed_row) + lv_obj_get_width(bed_row) <= lv_obj_get_width(h.root()));

    // Every value is shown whole: its label box sits inside its column, and the
    // column inside the widget's height.
    for (lv_obj_t* temp : {nozzle_temp, bed_temp}) {
        lv_obj_t* col = lv_obj_get_parent(temp);
        CHECK_FALSE(lv_obj_is_hidden(temp));
        CHECK(lv_obj_get_x(temp) >= 0);
        CHECK(lv_obj_get_x(temp) + lv_obj_get_width(temp) <= lv_obj_get_width(col));
        CHECK(lv_obj_get_y(col) + lv_obj_get_height(col) <= lv_obj_get_height(h.root()));
    }

    // With room, each value carries its caption underneath.
    CHECK_FALSE(lv_obj_is_hidden(nozzle_label));
    CHECK_FALSE(lv_obj_is_hidden(bed_label));
    CHECK(lv_obj_get_y(nozzle_label) >= lv_obj_get_y(nozzle_temp) + lv_obj_get_height(nozzle_temp));

    // Back to one cell: the rows stack again and the captions go.
    h.resize(2, 2, w_normal() - 1, h_tall() - 1);
    lv_obj_update_layout(h.root());
    CHECK(lv_obj_get_style_flex_flow(h.root(), LV_PART_MAIN) == LV_FLEX_FLOW_COLUMN);
    CHECK(lv_obj_is_hidden(nozzle_label));
    CHECK(lv_obj_get_y(nozzle_row) < lv_obj_get_y(bed_row));
}

// Home recycles widget instances: the tile is rebuilt and the same instance
// attached again, so the arrangement must follow the size it is granted next,
// not the one the previous tile ended on (prestonbrown/helixscreen#2198).
TEST_CASE_METHOD(LVGLUITestFixture, "temp_stack re-attached at another size gets that size's shape",
                 "[widget_size][temp_stack][panel_widget][wide]") {
    PanelWidgetHarness<TempStackWidget> h(test_screen(), "temp_stack", state(), nullptr);

    h.resize(4, 2, 2 * w_normal(), 4 * h_tall() / 3);
    REQUIRE(lv_obj_get_style_flex_flow(h.root(), LV_PART_MAIN) == LV_FLEX_FLOW_ROW);

    // Rebuild the tile and attach the same instance, then grant a one-cell box.
    lv_obj_t* old_tile = h.root();
    h.widget().detach_tile();
    lv_obj_delete(old_tile);
    h.create_tile(test_screen());
    h.resize(2, 2, w_normal() - 1, h_tall() - 1);
    lv_obj_update_layout(h.root());

    CHECK(lv_obj_get_style_flex_flow(h.root(), LV_PART_MAIN) == LV_FLEX_FLOW_COLUMN);
    CHECK(lv_obj_is_hidden(h.child("temp_stack_nozzle_label")));
    CHECK(lv_obj_get_y(h.child("temp_stack_nozzle_row")) <
          lv_obj_get_y(h.child("temp_stack_bed_row")));

    // And the other way: a stacked tile re-attached, then widened.
    old_tile = h.root();
    h.widget().detach_tile();
    lv_obj_delete(old_tile);
    h.create_tile(test_screen());
    h.resize(4, 2, 2 * w_normal(), 4 * h_tall() / 3);
    lv_obj_update_layout(h.root());
    CHECK(lv_obj_get_style_flex_flow(h.root(), LV_PART_MAIN) == LV_FLEX_FLOW_ROW);
    CHECK_FALSE(lv_obj_is_hidden(h.child("temp_stack_nozzle_label")));
}

TEST_CASE_METHOD(LVGLUITestFixture, "temp_stack: two tiles keep their own shape",
                 "[widget_size][temp_stack][panel_widget][wide]") {
    // Both instances exist before either tile is parsed, the order
    // PanelWidgetManager builds a page in, so a shared subject name would bind
    // both tiles to whichever was published last.
    const HarnessConfig deferred{nlohmann::json::object(), true};
    PanelWidgetHarness<TempStackWidget> wide(test_screen(), deferred, "temp_stack_wide", state(),
                                             nullptr);
    PanelWidgetHarness<TempStackWidget> narrow(test_screen(), deferred, "temp_stack_narrow",
                                               state(), nullptr);
    wide.create_tile(test_screen());
    narrow.create_tile(test_screen());

    wide.resize(4, 2, 2 * w_normal(), 4 * h_tall() / 3);
    narrow.resize(2, 2, w_normal() - 1, h_tall() - 1);
    // Sizing the second must not have re-shaped the first.
    lv_obj_update_layout(wide.root());
    lv_obj_update_layout(narrow.root());

    CHECK(lv_obj_get_style_flex_flow(wide.root(), LV_PART_MAIN) == LV_FLEX_FLOW_ROW);
    CHECK_FALSE(lv_obj_is_hidden(wide.child("temp_stack_nozzle_label")));
    CHECK(lv_obj_get_style_flex_flow(narrow.root(), LV_PART_MAIN) == LV_FLEX_FLOW_COLUMN);
    CHECK(lv_obj_is_hidden(narrow.child("temp_stack_nozzle_label")));
}

// 2x2: one row per heater down the box, each a big glyph with its value to the
// right, sized up to the box (prestonbrown/helixscreen#2198).
TEST_CASE_METHOD(LVGLUITestFixture, "temp_stack at 2x2 draws big rows, glyph left of value",
                 "[widget_size][temp_stack][panel_widget][wide]") {
    PanelWidgetHarness<TempStackWidget> h(test_screen(), "temp_stack", state(), nullptr);
    lv_obj_t* nozzle_row = h.child("temp_stack_nozzle_row");
    lv_obj_t* bed_row = h.child("temp_stack_bed_row");
    lv_obj_t* nozzle_glyph = h.child("nozzle_icon_glyph");
    lv_obj_t* nozzle_temp = h.child("temp_stack_nozzle_temp");

    // 2x2 cells = 4x4 tracks.
    h.resize(4, 4, 2 * w_normal(), 2 * h_tall());
    lv_obj_update_layout(h.root());

    CHECK(lv_obj_get_style_flex_flow(h.root(), LV_PART_MAIN) == LV_FLEX_FLOW_COLUMN);
    CHECK(lv_obj_get_style_flex_flow(nozzle_row, LV_PART_MAIN) == LV_FLEX_FLOW_ROW);
    CHECK(lv_obj_get_y(nozzle_row) + lv_obj_get_height(nozzle_row) <= lv_obj_get_y(bed_row));
    // Glyph left of its value, level with it, and larger than the 24px glyph a
    // one-cell box uses.
    CHECK(lv_obj_get_x(nozzle_glyph) + lv_obj_get_width(nozzle_glyph) <= lv_obj_get_x(nozzle_temp));
    CHECK(lv_obj_get_style_text_font(nozzle_glyph, LV_PART_MAIN)->line_height >
          mdi_icons_24.line_height);
    // Rows start at the left edge, so the glyphs line up in one column.
    CHECK(lv_obj_get_x(lv_obj_get_child(nozzle_row, 0)) ==
          lv_obj_get_x(lv_obj_get_child(bed_row, 0)));
    CHECK(lv_obj_get_style_flex_main_place(nozzle_row, LV_PART_MAIN) == LV_FLEX_ALIGN_START);
    // Every row fits the box.
    CHECK(lv_obj_get_y(h.child("temp_stack_bed_row")) + lv_obj_get_height(bed_row) <=
          lv_obj_get_height(h.root()));

    // A one-cell-wide tall box keeps the icon-over-value stack.
    h.resize(2, 4, w_normal() - 1, 2 * h_tall());
    lv_obj_update_layout(h.root());
    CHECK(lv_obj_get_style_flex_flow(nozzle_row, LV_PART_MAIN) == LV_FLEX_FLOW_COLUMN);
}

// Columns take the largest text tier whose three columns fit, so a wider box
// reads bigger than a narrower one (prestonbrown/helixscreen#2198).
TEST_CASE_METHOD(LVGLUITestFixture, "temp_stack columns use bigger text in a wider box",
                 "[widget_size][temp_stack][panel_widget][wide]") {
    PanelWidgetHarness<TempStackWidget> h(test_screen(), "temp_stack", state(), nullptr);
    lv_obj_t* temp = lv_obj_get_child(h.child("temp_stack_nozzle_row"), 1);
    auto text_px = [&] {
        return lv_obj_get_style_text_font(lv_obj_get_child(temp, 0), LV_PART_MAIN)->line_height;
    };

    // Sweep the width across the columns range: the text never shrinks as the
    // box widens, and the widest box reads bigger than the narrowest.
    const int box_h = 2 * h_tall();
    int first_px = 0;
    int last_px = 0;
    int widths_in_columns = 0;
    for (int w = w_normal(); w <= 6 * w_normal(); w += 4) {
        h.resize(6, 2, w, box_h);
        if (lv_obj_get_style_flex_flow(h.root(), LV_PART_MAIN) != LV_FLEX_FLOW_ROW)
            continue;
        const int px = text_px();
        INFO("width " << w);
        CHECK(px >= last_px);
        if (widths_in_columns++ == 0)
            first_px = px;
        last_px = px;
    }
    REQUIRE(widths_in_columns > 0);
    CHECK(last_px > first_px);
}

// Big rows leave the box's padding above the first row and below the last: a
// glyph that fits to the pixel reads as clipped against the edge. Swept across
// heights so every text tier's hand-off is covered.
TEST_CASE_METHOD(LVGLUITestFixture, "temp_stack big rows sit inside the padded box",
                 "[widget_size][temp_stack][panel_widget][wide]") {
    // The 2x2 grants of the 800x480 and 480x320 panels as they arrive at
    // runtime, under every tier whose text ladder they may meet, plus every
    // height around the first big-rows size (a square-ish box, so the 3:2
    // columns rule stays out of it). This also pins the widget passing a
    // nonzero margin to the classifier: with none, the tightest heights put the
    // rows against the box edge.
    std::vector<std::pair<int, int>> grants = {{233, 230}, {166, 155}};
    for (int width = 120; width <= 233; width += 7)
        for (int height = 90; height <= 260; height += 2)
            grants.emplace_back(width, height);

    int rows_seen = 0;
    for (UiBreakpoint bp :
         {UiBreakpoint::Tiny, UiBreakpoint::Small, UiBreakpoint::Medium, UiBreakpoint::Large}) {
        helix::test::ScopedBreakpoint tier(bp);
        PanelWidgetHarness<TempStackWidget> h(test_screen(), "temp_stack", state(), nullptr);
        const int pad = static_cast<int>(theme_manager_get_spacing("space_sm"));
        for (auto [width, height] : grants) {
            h.resize(4, 4, width, height);
            lv_obj_update_layout(h.root());
            lv_obj_t* first = h.child("temp_stack_nozzle_row");
            if (lv_obj_get_style_flex_flow(first, LV_PART_MAIN) != LV_FLEX_FLOW_ROW ||
                lv_obj_get_style_flex_main_place(first, LV_PART_MAIN) != LV_FLEX_ALIGN_START)
                continue; // not the big-rows layout in this box
            rows_seen++;
            INFO("tier " << to_int(bp) << " box " << width << "x" << height);
            lv_obj_t* last = h.child("temp_stack_chamber_row");
            if (lv_obj_is_hidden(last))
                last = h.child("temp_stack_bed_row");
            // The rows themselves, not their glyphs: the classifier budgets the
            // row heights, and a row is as tall as its largest part.
            CHECK(lv_obj_get_y(first) >= pad);
            CHECK(lv_obj_get_y(last) + lv_obj_get_height(last) <= height - pad);
        }
    }
    REQUIRE(rows_seen > 0);
}
