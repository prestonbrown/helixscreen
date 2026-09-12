// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_nozzle_temps_layout.cpp
 * @brief Unit tests for decide_nozzle_layout() — the pure layout-decision
 *        function behind the Nozzle Temps dashboard widget.
 *
 * decide_nozzle_layout() is deliberately LVGL-free, so these tests need no
 * display, font subsystem, or fixture. The decision is two independent
 * questions: the FONT TIER is chosen by height (the normal font only where
 * its whole stack — title + every row — fits the tile), and the LABEL RUNG by
 * width within the tier (the chosen spelling, then the bare number, then the
 * icon alone), with a width fall-through to the compact tier.
 *
 * The fixtures are the widths the widget really measured on an 800x480 panel
 * (values rendered as the rows show them), so the boundaries below are the
 * live ones: BASE is the idle state ("23.5° off" values) and HEATED is the
 * printing state ("/ 220°" values). WIDE models de/ru, where the position
 * label is the wider spelling.
 *
 * Run with: ./build/bin/helix-tests "[nozzle][layout]"
 */

#include "src/ui/panel_widgets/nozzle_layout.h"

#include "../catch_amalgamated.hpp"

using helix::decide_nozzle_layout;
using helix::NozzleLabelMode;
using helix::NozzleLayoutDecision;
using helix::NozzleRowWidths;
using helix::NozzleStackHeights;

namespace {

/// Whole-row widths for one font: the row spelled with the nozzle name, with
/// the position label, with the bare number, and with no label at all.
constexpr NozzleRowWidths widths(int long_px, int short_px, int number_px, int icon_px) {
    return {long_px, short_px, number_px, icon_px};
}

constexpr NozzleRowWidths BASE_NORMAL = widths(157, 138, 75, 60);
constexpr NozzleRowWidths BASE_COMPACT = widths(133, 118, 62, 48);
constexpr NozzleRowWidths HEATED_NORMAL = widths(187, 168, 68, 53);
constexpr NozzleRowWidths HEATED_COMPACT = widths(155, 140, 57, 43);
// de/ru: the position label is the wider spelling, so the long form wins at
// every width and the fallbacks have to work without swapping spellings.
constexpr NozzleRowWidths WIDE_NORMAL = widths(157, 207, 75, 60);
constexpr NozzleRowWidths WIDE_COMPACT = widths(127, 167, 62, 48);

/// Per-line stack heights as measured: base 15 (title + padding + rounding),
/// one row line + inter-row pad at each font. Five stacked items (4 extruders
/// + bed) come to 150px normal / 130px compact, the live numbers.
constexpr NozzleStackHeights TALL{15, 27, 23};

/// The unknown-height (pre-layout) contract: no vertical constraint.
constexpr int ANY_H = 0;

} // namespace

// --- Column split: two columns when the narrower spelling fits twice.

TEST_CASE("Narrow 480px-panel tile stays one column holding the short spelling",
          "[nozzle][layout]") {
    // 2*138 + 12 = 288 > 196 → one column, wide enough for the long row.
    NozzleLayoutDecision d =
        decide_nozzle_layout(196, ANY_H, 12, BASE_NORMAL, BASE_COMPACT, 4, TALL);
    REQUIRE(d.columns == 1);
    REQUIRE(d.label_mode == NozzleLabelMode::Long);
    CHECK(d.use_compact_font == false);
}

TEST_CASE("Two short columns when both plus the gap fit", "[nozzle][layout]") {
    // 2*138 + 12 = 288 <= avail → two columns of 138, each holding short(138)
    // exactly at the split boundary.
    NozzleLayoutDecision d =
        decide_nozzle_layout(288, ANY_H, 12, BASE_NORMAL, BASE_COMPACT, 4, TALL);
    REQUIRE(d.columns == 2);
    REQUIRE(d.label_mode == NozzleLabelMode::Short);
}

TEST_CASE("Single extruder never splits into two columns", "[nozzle][layout]") {
    NozzleLayoutDecision d =
        decide_nozzle_layout(500, ANY_H, 12, BASE_NORMAL, BASE_COMPACT, 1, TALL);
    REQUIRE(d.columns == 1);
    REQUIRE(d.label_mode == NozzleLabelMode::Long);
}

TEST_CASE("One pixel below the split boundary stays a single column", "[nozzle][layout]") {
    // One pixel under the 2*138 + 12 = 288 split → single column, holds long.
    NozzleLayoutDecision d =
        decide_nozzle_layout(287, ANY_H, 12, BASE_NORMAL, BASE_COMPACT, 4, TALL);
    REQUIRE(d.columns == 1);
    REQUIRE(d.label_mode == NozzleLabelMode::Long);
}

TEST_CASE("Zero rows clamps to a single column", "[nozzle][layout]") {
    NozzleLayoutDecision d =
        decide_nozzle_layout(500, ANY_H, 12, BASE_NORMAL, BASE_COMPACT, 0, TALL);
    REQUIRE(d.columns == 1);
}

TEST_CASE("Degenerate width returns the safe single-column long default", "[nozzle][layout]") {
    NozzleLayoutDecision d = decide_nozzle_layout(0, ANY_H, 12, BASE_NORMAL, BASE_COMPACT, 4, TALL);
    REQUIRE(d.columns == 1);
    REQUIRE(d.label_mode == NozzleLabelMode::Long);
    CHECK(d.use_compact_font == false);
    d = decide_nozzle_layout(-50, ANY_H, 12, BASE_NORMAL, BASE_COMPACT, 4, TALL);
    REQUIRE(d.label_mode == NozzleLabelMode::Long);
    CHECK(d.use_compact_font == false);
}

// --- The label rung is a width decision within the tier: the spelling, then
// the bare number, then the icon; a number always beats the icon alone, and a
// spelling always beats a number, once the font is settled.

TEST_CASE("A wide column keeps the long spelling at the normal font", "[nozzle][layout]") {
    NozzleLayoutDecision d =
        decide_nozzle_layout(200, ANY_H, 12, BASE_NORMAL, BASE_COMPACT, 4, TALL);
    REQUIRE(d.columns == 1);
    REQUIRE(d.label_mode == NozzleLabelMode::Long);
    CHECK(d.use_compact_font == false);
}

TEST_CASE("One pixel below the long spelling uses the short spelling", "[nozzle][layout]") {
    NozzleLayoutDecision d =
        decide_nozzle_layout(156, ANY_H, 12, BASE_NORMAL, BASE_COMPACT, 4, TALL);
    REQUIRE(d.label_mode == NozzleLabelMode::Short);
    CHECK(d.use_compact_font == false);
}

TEST_CASE("Equal-width forms keep the long label when neither fits", "[nozzle][layout]") {
    NozzleLayoutDecision d = decide_nozzle_layout(160, ANY_H, 12, widths(157, 157, 88, 73),
                                                  widths(133, 133, 82, 69), 4, TALL);
    REQUIRE(d.label_mode == NozzleLabelMode::Long);
}

TEST_CASE("The normal tier's number outranks the compact tier's spelling", "[nozzle][layout]") {
    // WIDE shape at 130: no spelling fits at the normal font (157/207), the
    // number does (88). Height allows the normal tier, so the number at the
    // normal font wins before any compact rung is even consulted — the font a
    // row is drawn in follows the tile's height, not its width.
    NozzleLayoutDecision d =
        decide_nozzle_layout(130, ANY_H, 12, WIDE_NORMAL, WIDE_COMPACT, 4, TALL);
    REQUIRE(d.label_mode == NozzleLabelMode::Number);
    CHECK(d.use_compact_font == false);
}

TEST_CASE("Two columns are gated on the narrower spelling, not the compact one",
          "[nozzle][layout]") {
    // de: 2*long(150) + 12 = 312 <= 400 → two columns of 194 >= 150 long.
    NozzleLayoutDecision d =
        decide_nozzle_layout(400, ANY_H, 12, WIDE_NORMAL, WIDE_COMPACT, 4, TALL);
    REQUIRE(d.columns == 2);
    REQUIRE(d.label_mode == NozzleLabelMode::Long);
}

// --- The font tier is a height decision: the normal font only where its
// whole stack fits the tile.

TEST_CASE("A short tile drops to the compact font however wide it is", "[nozzle][layout][1613]") {
    // Width alone would keep the long spelling at the normal font (200 >=
    // 150), but the normal stack (150) does not fit 120 of height; the
    // compact stack (130) does.
    NozzleLayoutDecision d = decide_nozzle_layout(200, 120, 12, BASE_NORMAL, BASE_COMPACT, 4, TALL);
    REQUIRE(d.label_mode == NozzleLabelMode::Long);
    CHECK(d.use_compact_font == true);
}

TEST_CASE("A tall tile keeps the normal font at the same width", "[nozzle][layout]") {
    NozzleLayoutDecision d = decide_nozzle_layout(200, 160, 12, BASE_NORMAL, BASE_COMPACT, 4, TALL);
    REQUIRE(d.label_mode == NozzleLabelMode::Long);
    CHECK(d.use_compact_font == false);
}

TEST_CASE("The height boundary is exact", "[nozzle][layout]") {
    CHECK(decide_nozzle_layout(200, 150, 12, BASE_NORMAL, BASE_COMPACT, 4, TALL).use_compact_font ==
          false);
    CHECK(decide_nozzle_layout(200, 149, 12, BASE_NORMAL, BASE_COMPACT, 4, TALL).use_compact_font ==
          true);
}

TEST_CASE("An unknown height imposes no vertical constraint", "[nozzle][layout]") {
    NozzleLayoutDecision d =
        decide_nozzle_layout(200, ANY_H, 12, BASE_NORMAL, BASE_COMPACT, 4, TALL);
    CHECK(d.use_compact_font == false);
}

// --- Height picks the tier, width picks the rung: raising the tile's height
// grows the font even where that trades a spelling down to a number, because
// a row is one line tall whatever it says.

TEST_CASE("Growing the height grows the font at the 2-unit column", "[nozzle][layout][1613]") {
    // The reported geometry family: 104 wide. Idle values (BASE): the number
    // row at the normal font is 98 <= 104; the short spelling is not (131).
    //
    // TINY height (102 < compact stack 130): compact tier → number (85 <=
    // 104). Same label, small font.
    NozzleLayoutDecision tiny =
        decide_nozzle_layout(104, 102, 12, BASE_NORMAL, BASE_COMPACT, 4, TALL);
    CHECK(tiny.label_mode == NozzleLabelMode::Number);
    CHECK(tiny.use_compact_font == true);

    // Rowspan 3 (161 >= 150): the normal tier opens, and its first rung the
    // column holds is the NUMBER at the normal font — the font grows.
    NozzleLayoutDecision grown =
        decide_nozzle_layout(104, 161, 12, BASE_NORMAL, BASE_COMPACT, 4, TALL);
    CHECK(grown.label_mode == NozzleLabelMode::Number);
    CHECK(grown.use_compact_font == false);
}

TEST_CASE("The number at the normal font beats the spelling at the compact font",
          "[nozzle][layout]") {
    // Tall tile, column 120: the normal tier holds the number row (98) even
    // though it holds neither spelling — so the number at the NORMAL font
    // wins before any compact rung is consulted.
    NozzleLayoutDecision d =
        decide_nozzle_layout(120, ANY_H, 12, BASE_NORMAL, BASE_COMPACT, 4, TALL);
    REQUIRE(d.label_mode == NozzleLabelMode::Number);
    CHECK(d.use_compact_font == false);

    // Below the normal tier's icon row (60): the compact tier's icon (48) is
    // the floor — the compact number row (62) sits above the normal icon, so
    // that band always belongs to the normal tier's icon.
    NozzleLayoutDecision narrower =
        decide_nozzle_layout(55, ANY_H, 12, BASE_NORMAL, BASE_COMPACT, 4, TALL);
    CHECK(narrower.label_mode == NozzleLabelMode::None);
    CHECK(narrower.use_compact_font == true);
}

TEST_CASE("The icon rung only serves where nothing labeled fits", "[nozzle][layout]") {
    // The number row budgets the current-only value, so the icon rung — 15px
    // narrower — only serves below it: at 59 nothing in the normal tier fits
    // (spelling 138/157, number 75, icon 60), the compact number (62) misses
    // too, and the compact icon (48 <= 59) is the floor.
    NozzleLayoutDecision floor =
        decide_nozzle_layout(59, ANY_H, 12, BASE_NORMAL, BASE_COMPACT, 4, TALL);
    CHECK(floor.label_mode == NozzleLabelMode::None);
    CHECK(floor.use_compact_font == true);
}

TEST_CASE("Below the compact icon row there is nothing left to drop", "[nozzle][layout]") {
    NozzleLayoutDecision d =
        decide_nozzle_layout(42, ANY_H, 12, BASE_NORMAL, BASE_COMPACT, 4, TALL);
    CHECK(d.label_mode == NozzleLabelMode::None);
    CHECK(d.use_compact_font == true);
}

// --- The reported tile: 104px wide, both spellings long (ru), values heated.
// This is the whole reason the number rung exists.

TEST_CASE("The reported one-grid-square Russian tile shows the number", "[nozzle][layout][1613]") {
    // ru heated: neither spelling fits at either font (168/207 normal,
    // 140/167 compact), but the number row — budgeting the current-only
    // value — fits at the normal font (68 <= 104) and the tile is tall, so
    // the reported tile shows number + current temp at the FULL font.
    NozzleLayoutDecision d =
        decide_nozzle_layout(104, 215, 12, HEATED_NORMAL, HEATED_COMPACT, 4, TALL);
    REQUIRE(d.columns == 1);
    CHECK(d.label_mode == NozzleLabelMode::Number);
    CHECK(d.use_compact_font == false);
}

TEST_CASE("Heated values trade the spelling down to the number, same tier", "[nozzle][layout]") {
    // A 3-unit column (163): idle holds the short spelling at the normal
    // font; heated widens it past the column and the number row takes over —
    // still the normal font, still a label, no overlap.
    NozzleLayoutDecision idle =
        decide_nozzle_layout(163, 215, 12, BASE_NORMAL, BASE_COMPACT, 4, TALL);
    CHECK(idle.label_mode == NozzleLabelMode::Long);
    CHECK(idle.use_compact_font == false);
    NozzleLayoutDecision heated =
        decide_nozzle_layout(163, 215, 12, HEATED_NORMAL, HEATED_COMPACT, 4, TALL);
    CHECK(heated.label_mode == NozzleLabelMode::Number);
    CHECK(heated.use_compact_font == false);
}

TEST_CASE("A tile shorter than both stacks keeps the best label the column fits",
          "[nozzle][layout]") {
    // 104x102: the compact stack (130) overflows 102, but a labeled row is
    // the same height as an unlabeled one, so the ladder still serves the
    // number at the compact font rather than dropping to icons.
    NozzleLayoutDecision d = decide_nozzle_layout(104, 102, 12, BASE_NORMAL, BASE_COMPACT, 4, TALL);
    CHECK(d.label_mode == NozzleLabelMode::Number);
    CHECK(d.use_compact_font == true);
}

TEST_CASE("Two columns never shrink the font", "[nozzle][layout]") {
    // The split is gated on the narrower spelling fitting twice at the normal
    // font, so a two-column layout is always a normal-tier layout.
    NozzleLayoutDecision d =
        decide_nozzle_layout(288, ANY_H, 12, BASE_NORMAL, BASE_COMPACT, 4, TALL);
    REQUIRE(d.columns == 2);
    CHECK(d.use_compact_font == false);
    CHECK(d.label_mode == NozzleLabelMode::Short);
}

TEST_CASE("Two columns wrap the stack, keeping the normal tier on a short tile",
          "[nozzle][layout]") {
    // A wide tile whose single-column stack would overflow: 288 wide splits
    // into two columns, the five items wrap to 3 lines (96px at the normal
    // font), and 110 of height is enough for the wrapped stack where the
    // single-column model (150) would have vetoed the normal tier.
    NozzleLayoutDecision d = decide_nozzle_layout(288, 110, 12, BASE_NORMAL, BASE_COMPACT, 4, TALL);
    REQUIRE(d.columns == 2);
    CHECK(d.label_mode == NozzleLabelMode::Short);
    CHECK(d.use_compact_font == false);
}
