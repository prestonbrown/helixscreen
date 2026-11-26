// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_stacked_row_layout.cpp
 * @brief The temp_stack tall-layout decision, exercised without a display.
 *
 * wants_icon_above() is the whole rule for the icon-over-temperature
 * layout: an authored height of more than one grid cell AND enough measured
 * pixels for every visible heater to draw two lines. Both halves matter and
 * both are asserted here — the rowspan gate alone would promote a 3-track
 * widget on a Micro panel that has nowhere to put the second line, and the
 * pixel test alone would promote a one-cell widget on XXLarge, where a single
 * cell is tall enough to fit three stacked pairs.
 *
 * The widget-level counterpart (fonts and flex_flow actually applied to the
 * rows) is tests/unit/test_widget_size_temp_stack.cpp.
 */

#include "src/ui/panel_widgets/stacked_row_layout.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {
// A representative Small-tier row: 24px icon glyph, 12px temperature text,
// 2px between them.
constexpr StackedRowMetrics kSmallRow{24, 12, 2};
constexpr int kRowGap = 4;

// Height that exactly fits `rows` stacked heaters at kSmallRow.
constexpr int exact_fit(int rows) {
    return rows * (kSmallRow.icon_px + kSmallRow.icon_gap_px + kSmallRow.text_px) +
           (rows - 1) * kRowGap;
}
} // namespace

TEST_CASE("temp_stack tall layout needs more than one cell of authored height",
          "[widget_size][temp_stack][layout]") {
    // Plenty of pixels, so rowspan is the only thing under test.
    const int roomy = exact_fit(3) * 4;

    // The default 2x2-track size is one grid cell — rows stay side by side
    // there no matter how many pixels that cell happens to be worth.
    CHECK_FALSE(wants_icon_above(2, roomy, 3, kSmallRow, kRowGap));
    CHECK_FALSE(wants_icon_above(1, roomy, 3, kSmallRow, kRowGap));

    // 3 tracks (1.5 cells) is the first size that qualifies; 4 (the widget's
    // max_rowspan) also does.
    CHECK(wants_icon_above(3, roomy, 3, kSmallRow, kRowGap));
    CHECK(wants_icon_above(4, roomy, 3, kSmallRow, kRowGap));

    // The gate is pinned to the half-cell grid, not to a bare literal.
    STATIC_REQUIRE(kIconAboveMinRowspan == 3);
}

TEST_CASE("temp_stack tall layout needs pixels for every visible heater",
          "[widget_size][temp_stack][layout]") {
    // Exactly enough is enough; one pixel short is not.
    CHECK(wants_icon_above(4, exact_fit(3), 3, kSmallRow, kRowGap));
    CHECK_FALSE(wants_icon_above(4, exact_fit(3) - 1, 3, kSmallRow, kRowGap));

    // A printer with no chamber sensor hides that row, so the same height that
    // could not fit three heaters comfortably fits two. Dropping the visible
    // count must be what changes the verdict, not the height.
    const int two_row_height = exact_fit(2);
    CHECK_FALSE(wants_icon_above(3, two_row_height, 3, kSmallRow, kRowGap));
    CHECK(wants_icon_above(3, two_row_height, 2, kSmallRow, kRowGap));
}

TEST_CASE("temp_stack tall layout falls back to rows when it cannot measure",
          "[widget_size][temp_stack][layout]") {
    const int roomy = exact_fit(3) * 4;

    // Pre-layout: the widget has been created but never sized.
    CHECK_FALSE(wants_icon_above(4, 0, 3, kSmallRow, kRowGap));
    CHECK_FALSE(wants_icon_above(4, -1, 3, kSmallRow, kRowGap));

    // Every heater row hidden — nothing to lay out either way.
    CHECK_FALSE(wants_icon_above(4, roomy, 0, kSmallRow, kRowGap));

    // Fonts not resolved yet: a zero line height would make any height look
    // roomy enough, so an unmeasurable row must decline instead.
    CHECK_FALSE(wants_icon_above(4, roomy, 3, StackedRowMetrics{0, 12, 2}, kRowGap));
    CHECK_FALSE(wants_icon_above(4, roomy, 3, StackedRowMetrics{24, 0, 2}, kRowGap));
}

namespace {
// Small-tier row with a 10px caption under the value, and columns 40px wide
// bare / 60px with the caption.
constexpr StackedRowMetrics kCaptionRow{24, 12, 2, 10};
constexpr ColumnWidths kCols{40, 60, 100};
constexpr int kColGap = 4;
constexpr int kBareHeight = 24 + 2 + 12;
constexpr int kCaptionHeight = kBareHeight + 2 + 10;
constexpr int widthFor(int rows, int column_px) {
    return rows * column_px + (rows - 1) * kColGap;
}
} // namespace

TEST_CASE("stack layout lays rows side by side only from 1.5 cells wide",
          "[widget_size][stack_layout][layout]") {
    const int w = widthFor(3, kCols.captioned_px);

    // One whole cell wide never goes to columns, however many pixels it has.
    CHECK(classify_stack_layout(2, 2, w * 4, 400, 3, kCaptionRow, kColGap, kCols).shape !=
          StackShape::Columns);
    CHECK(classify_stack_layout(kColumnsMinColspan, 2, w, 400, 3, kCaptionRow, kColGap, kCols)
              .shape == StackShape::Columns);
    CHECK(classify_stack_layout(4, 2, w, 400, 3, kCaptionRow, kColGap, kCols).shape ==
          StackShape::Columns);
    STATIC_REQUIRE(kColumnsMinColspan == 3);
}

TEST_CASE("stack layout drops the caption before it drops the columns",
          "[widget_size][stack_layout][layout]") {
    const int w = widthFor(3, kCols.captioned_px);

    auto at = [&](int width, int height) {
        return classify_stack_layout(4, 2, width, height, 3, kCaptionRow, kColGap, kCols);
    };
    auto full = at(w, kCaptionHeight);
    CHECK(full.shape == StackShape::Columns);
    CHECK(full.captions);

    // One pixel short of the caption's height: columns stay, caption goes.
    auto short_box = at(w, kCaptionHeight - 1);
    CHECK(short_box.shape == StackShape::Columns);
    CHECK_FALSE(short_box.captions);

    // One pixel short of the caption's width: same.
    auto narrow = at(w - 1, kCaptionHeight);
    CHECK(narrow.shape == StackShape::Columns);
    CHECK_FALSE(narrow.captions);

    // Too narrow for even bare columns, or too short for icon and value: the
    // vertical rule decides, exactly as without a width.
    CHECK(at(widthFor(3, kCols.plain_px) - 1, 400).shape != StackShape::Columns);
    CHECK(at(w, kBareHeight - 1).shape != StackShape::Columns);
}

TEST_CASE("stack layout columns need the physical width band as well as the span",
          "[widget_size][stack_layout][layout]") {
    // Plenty for the columns themselves, but under the band: pixels win.
    const int w = widthFor(3, kCols.captioned_px);
    REQUIRE(w >= kCols.min_box_px);
    CHECK(classify_stack_layout(4, 2, kCols.min_box_px - 1, 400, 2, kCaptionRow, kColGap, kCols)
              .shape != StackShape::Columns);
    CHECK(
        classify_stack_layout(4, 2, kCols.min_box_px, 400, 2, kCaptionRow, kColGap, kCols).shape ==
        StackShape::Columns);
}

TEST_CASE("stack layout keeps the vertical stack in a box that is tall as well as wide",
          "[widget_size][stack_layout][layout]") {
    const int w = widthFor(3, kCols.captioned_px);
    const int tall = exact_fit(3) * 4;
    REQUIRE(wants_icon_above(4, tall, 3, kCaptionRow, kColGap));

    // Roughly square and tall enough to stack: the vertical stack stays.
    CHECK(classify_stack_layout(4, 4, w, w, 3, kCaptionRow, kColGap, kCols).shape ==
          StackShape::IconAbove);
    // Clearly wider than tall (3:2 and up): columns, even where stacking fits.
    CHECK(classify_stack_layout(4, 4, w, w * 2 / 3, 3, kCaptionRow, kColGap, kCols).shape ==
          StackShape::Columns);
}

TEST_CASE("stack layout columns decline when it cannot measure",
          "[widget_size][stack_layout][layout]") {
    const int w = widthFor(3, kCols.captioned_px);
    CHECK(classify_stack_layout(4, 2, 0, 400, 3, kCaptionRow, kColGap, kCols).shape !=
          StackShape::Columns);
    CHECK(classify_stack_layout(4, 2, w, 400, 1, kCaptionRow, kColGap, kCols).shape !=
          StackShape::Columns);
    CHECK(classify_stack_layout(4, 2, w, 400, 3, kCaptionRow, kColGap, ColumnWidths{}).shape !=
          StackShape::Columns);
}

TEST_CASE("stack layout gives a square, tall box big rows, and a narrow one the icon stack",
          "[widget_size][stack_layout][layout]") {
    // Per-row width: 24px icon, then a 40px value, then a 50px caption, 4px apart.
    ColumnWidths cols{40, 60, 100, 24 + 4 + 40, 24 + 4 + 40 + 4 + 50};
    const int rows_h = 3 * 24 + 2 * kColGap; // the icon is the taller of icon and text
    const int h = exact_fit(3) * 4;
    REQUIRE(h >= rows_h);

    // Square: not wide enough for the 3:2 columns rule, so big rows.
    auto big = classify_stack_layout(4, 4, 200, 200, 3, kCaptionRow, kColGap, cols);
    CHECK(big.shape == StackShape::Rows);
    CHECK(big.captions);

    // The caption goes before the rows do.
    auto no_caption = classify_stack_layout(4, 4, cols.beside_captioned_px - 1, 200, 3, kCaptionRow,
                                            kColGap, cols);
    CHECK(no_caption.shape == StackShape::Rows);
    CHECK_FALSE(no_caption.captions);

    // Narrower than a row needs, or a one-cell-wide box: the vertical icon stack.
    CHECK(classify_stack_layout(4, 4, cols.beside_plain_px - 1, 200, 3, kCaptionRow, kColGap, cols)
              .shape == StackShape::IconAbove);
    CHECK(classify_stack_layout(2, 4, 300, 200, 3, kCaptionRow, kColGap, cols).shape ==
          StackShape::IconAbove);

    // One authored cell tall never goes to big rows, and a widget that measured
    // no beside width never offers them.
    CHECK(classify_stack_layout(4, 2, 300, 200, 3, kCaptionRow, kColGap, cols).shape !=
          StackShape::Rows);
    CHECK(classify_stack_layout(4, 4, 200, 200, 3, kCaptionRow, kColGap, kCols).shape !=
          StackShape::Rows);
}
