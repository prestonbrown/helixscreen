// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_panel_widget_size.cpp
 * @brief Pins the four physical size bands against real measured cell extents,
 * not against themselves.
 *
 * Every per-widget `[widget_size]` test drives its cases from
 * `w_normal() - 1` / `h_tall()` / etc., which proves a widget's own predicate
 * is wired to the band, but shifts both sides of the comparison together if
 * the band itself is miscalibrated — it cannot catch an off-by-one in the
 * band. This file drives the bands from the independently measured tier table
 * instead: real per-tier cell/span pixel extents, truncated the same way
 * `grid_track_extent()` truncates them at runtime
 * (`static_cast<int>(float)`, i.e. floor for these always-positive values).
 *
 * Every case names the tier its extent was measured on. A band is a per-tier
 * number — the ladder scales with font_body — so comparing one tier's measured
 * pixels against another tier's band answers a question no panel ever asks.
 *
 * All four predicates in this codebase compare with `>=`, so a band must be no
 * higher than the smallest extent that should match — one pixel low silently
 * admits a false positive (Defect 1: XLarge's 134px single-column width
 * exactly equalled a flat 134px W_NORMAL, so a plain 1x1 widget there read as
 * "wide"), never a false negative.
 */

#include "ui_breakpoint.h"

#include "panel_widget_size.h"

#include "catch_amalgamated.hpp"

using namespace helix::widget_size;

namespace {

/// A measured extent and the tier whose panel produced it.
struct TierExtent {
    const char* name;
    UiBreakpoint bp;
    int px;
};

} // namespace

TEST_CASE("w_normal admits the smallest genuine 2-column width and excludes every "
          "single-column width",
          "[widget_size][panel_widget_size]") {
    // Smallest measured 2-column (colspan>=2) extent across all eight tiers:
    // Small (480x400), span2 = 3*65.6667 rounds out to 135.33f truncated to
    // 135. This is the tightest genuine "wide" case there is — it must fire,
    // and it is the value the whole ladder is derived from.
    CHECK(135 >= w_normal(UiBreakpoint::Small));

    // Every tier's single-column (1x1) width, truncated exactly as
    // grid_track_extent() truncates it at runtime, against that tier's own
    // band. None of these are a colspan>=2 case and must NOT read as
    // w_normal-or-above. XLarge's 134 is Defect 1 itself.
    const TierExtent span1_by_tier[] = {
        {"micro", UiBreakpoint::Micro, 70},  {"tiny", UiBreakpoint::Tiny, 68},
        {"small", UiBreakpoint::Small, 65},  {"medium", UiBreakpoint::Medium, 114},
        {"large", UiBreakpoint::Large, 107}, {"xlarge", UiBreakpoint::XLarge, 134},
        // Portrait's 152 (480x800, Small tier) is intentionally excluded: the
        // header comment in panel_widget_size.h documents that physical size
        // legitimately overtakes span there (a single portrait column is wider
        // than some smaller tiers' two), so 152 >= w_normal(Small) is correct,
        // not a bug.
        //
        // Micro portrait (272x480) is excluded for the same reason and is
        // checked below: its column axis gains a track, so what it calls one
        // column is four tracks wide.
    };
    for (const auto& c : span1_by_tier) {
        INFO(c.name << " single column " << c.px << "px vs band " << w_normal(c.bp));
        CHECK(c.px < w_normal(c.bp));
    }

    // Micro portrait's 131px column reads as normal-or-above, and should. It
    // is a four-track column against a 12px type ladder — 10.9 em of width,
    // where Small's calibration point is 135px against 14px type, 9.6 em. The
    // narrower box holds more text, so the roomier layout is the right call.
    CHECK(131 >= w_normal(UiBreakpoint::Micro));
}

TEST_CASE("w_wide admits the smallest genuine 3-column width and excludes narrower spans",
          "[widget_size][panel_widget_size]") {
    // Smallest measured 3-column (colspan>=3) extent: Small, span3 = 205
    // (exact — no truncation slop at this tier's span3).
    CHECK(205 >= w_wide(UiBreakpoint::Small));

    // A representative 2-column width that must stay below w_wide on tiers
    // where colspan>=3 is meaningfully distinct from colspan>=2.
    CHECK(142 < w_wide(UiBreakpoint::Micro)); // Micro span2
}

TEST_CASE("h_tall admits the smallest genuine 2-row height and excludes every single-row height",
          "[widget_size][panel_widget_size]") {
    // Smallest measured 2-row (rowspan>=2) extent: Micro (480x272), row2 = 131
    // (exact). The ladder puts Micro's band at 112, so the tightest genuine
    // 2-row case still fires with room to spare.
    CHECK(131 >= h_tall(UiBreakpoint::Micro));

    // Every tier's single-row (1x1) height against that tier's own band,
    // truncated exactly as grid_track_extent() truncates it at runtime.
    //
    // Large's 141 and XLarge's 169 are Defect 2: against a flat 131px band a
    // single grid row on those tiers read as two, because their rows are
    // absolutely taller than Micro's genuine 2-row extent. Against their own
    // tiers' bands they do not, so a 1x1 is a 1x1 on every tier.
    const TierExtent row1_by_tier[] = {
        {"micro", UiBreakpoint::Micro, 64},  {"tiny", UiBreakpoint::Tiny, 76},
        {"small", UiBreakpoint::Small, 94},  {"medium", UiBreakpoint::Medium, 112},
        {"large", UiBreakpoint::Large, 141}, {"xlarge", UiBreakpoint::XLarge, 169},
    };
    for (const auto& c : row1_by_tier) {
        INFO(c.name << " single row " << c.px << "px vs band " << h_tall(c.bp));
        CHECK(c.px < h_tall(c.bp));
    }
}

TEST_CASE("h_taller already reflects truncation, not the raw float midpoint",
          "[widget_size][panel_widget_size]") {
    // Smallest measured 3-row (rowspan>=3) extent: Micro, row3 = 3*64.5 +
    // 2*2 = 197.5f exactly in float arithmetic. grid_track_extent()'s only
    // conversion to the int a widget receives is static_cast<int>(197.5f),
    // which truncates (not rounds) to 197 -- so 197, not 198 or 197.5, is
    // the real smallest value a widget ever sees at runtime, and the Small
    // rung of the ladder is that measurement under the >= rule.
    constexpr int micro_row3_raw_truncated = static_cast<int>(3.0f * 64.5f + 2.0f * 2.0f);
    CHECK(micro_row3_raw_truncated == 197);
    CHECK(micro_row3_raw_truncated >= h_taller(UiBreakpoint::Micro));
    CHECK(h_taller(UiBreakpoint::Small) == 197);
}

TEST_CASE("size bands scale with the font_body ladder", "[widget_size][panel_widget_size]") {
    // The whole point of the ladder: a band is "room for the roomy layout",
    // and what fills that room is text. Pinned per tier so a font ladder edit
    // that forgets these has to be an explicit decision.
    CHECK(w_normal(UiBreakpoint::Micro) == 116);
    CHECK(w_normal(UiBreakpoint::Tiny) == 116);
    CHECK(w_normal(UiBreakpoint::Small) == 135);
    CHECK(w_normal(UiBreakpoint::Medium) == 174);
    CHECK(w_normal(UiBreakpoint::Large) == 193);
    CHECK(w_normal(UiBreakpoint::XLarge) == 231);
    CHECK(w_normal(UiBreakpoint::XXLarge) == 309);

    CHECK(w_wide(UiBreakpoint::Micro) == 176);
    CHECK(w_wide(UiBreakpoint::Small) == 205);
    CHECK(w_wide(UiBreakpoint::XXLarge) == 469);

    CHECK(h_tall(UiBreakpoint::Micro) == 112);
    CHECK(h_tall(UiBreakpoint::Small) == 131);
    CHECK(h_tall(UiBreakpoint::XXLarge) == 299);

    CHECK(h_taller(UiBreakpoint::Micro) == 169);
    CHECK(h_taller(UiBreakpoint::Small) == 197);
    CHECK(h_taller(UiBreakpoint::XXLarge) == 450);

    // The XXLarge rung is what the flat bands got wrong: a one-cell widget
    // there is about 182px wide (GRID_CELL 96, two tracks plus a gutter) and
    // cleared a flat 135px band, so it chose the layout calibrated for 14px
    // type while drawing 32px type into it.
    CHECK(182 < w_normal(UiBreakpoint::XXLarge));
}

TEST_CASE("the legacy flat bands are the Small rung of their ladders",
          "[widget_size][panel_widget_size]") {
    // W_NORMAL/W_WIDE/H_TALL/H_TALLER are what the not-yet-ported call sites
    // still read. They have to stay exactly the Small rung, or porting a call
    // site would silently move its behavior on the tier the numbers were
    // measured on — which is the one tier this change is NOT meant to touch.
    CHECK(W_NORMAL == w_normal(BASE_TIER));
    CHECK(W_WIDE == w_wide(BASE_TIER));
    CHECK(H_TALL == h_tall(BASE_TIER));
    CHECK(H_TALLER == h_taller(BASE_TIER));
}

TEST_CASE("band accessors clamp an out-of-range tier instead of reading off the table",
          "[widget_size][panel_widget_size]") {
    // as_breakpoint() clamps, but the accessors also take a raw UiBreakpoint a
    // caller could have cast from an arbitrary int (the ui_breakpoint subject
    // is an int). Reading a seven-entry table with it must not walk off either
    // end.
    const auto below = static_cast<UiBreakpoint>(-3);
    const auto above = static_cast<UiBreakpoint>(99);

    CHECK(w_normal(below) == w_normal(UiBreakpoint::Micro));
    CHECK(w_wide(below) == w_wide(UiBreakpoint::Micro));
    CHECK(h_tall(below) == h_tall(UiBreakpoint::Micro));
    CHECK(h_taller(below) == h_taller(UiBreakpoint::Micro));

    CHECK(w_normal(above) == w_normal(UiBreakpoint::XXLarge));
    CHECK(w_wide(above) == w_wide(UiBreakpoint::XXLarge));
    CHECK(h_tall(above) == h_tall(UiBreakpoint::XXLarge));
    CHECK(h_taller(above) == h_taller(UiBreakpoint::XXLarge));
}
