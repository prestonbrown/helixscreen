// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_context_menu_position.cpp
 * @brief Placement rules for context-menu cards
 *
 * Four separate implementations of this arithmetic had accumulated - two copies
 * of a widget-anchored positioner that differed only by a string literal, a third
 * inlined copy, and a click-anchored one in the base class - and they disagreed
 * on the edge cases that matter: whether the card flips above its anchor, and
 * which edge gets clamped last when the card does not fit at all.
 *
 * The clamp order is the load-bearing part. Clamping the low edge last means an
 * oversized card overflows off the bottom (scrollable, recoverable) instead of
 * being pushed to a negative origin, which clips its header and first row off the
 * top where nothing can reach them.
 */

#include "ui_context_menu.h"

#include "../catch_amalgamated.hpp"

using helix::ui::ContextMenu;
using AnchorMode = ContextMenu::AnchorMode;
using AnchorAlign = ContextMenu::AnchorAlign;

namespace {

constexpr int32_t kMargin = 10;
constexpr int32_t kGap = 4;

lv_point_t below(lv_point_t card, lv_area_t anchor, lv_point_t bounds,
                 AnchorAlign align = AnchorAlign::Center) {
    return ContextMenu::compute_card_pos(card, anchor, bounds, kMargin, kGap,
                                         AnchorMode::BelowAnchor, align);
}

lv_point_t at_click(lv_point_t card, int32_t x, int32_t y, lv_point_t bounds) {
    return ContextMenu::compute_card_pos(card, {x, y, x, y}, bounds, kMargin, kGap,
                                         AnchorMode::ClickPoint, AnchorAlign::Center);
}

} // namespace

// ============================================================================
// BelowAnchor
// ============================================================================

TEST_CASE("compute_card_pos: card sits centred under its anchor", "[context-menu][position]") {
    // Anchor 100..200 wide, so its midpoint is 150; an 80px card starts at 110.
    const lv_point_t pos = below({80, 60}, {100, 100, 200, 140}, {800, 480});
    CHECK(pos.x == 110);
    CHECK(pos.y == 144); // anchor bottom + gap
}

TEST_CASE("compute_card_pos: left-aligned card starts at the anchor's left edge",
          "[context-menu][position]") {
    const lv_point_t pos = below({80, 60}, {100, 100, 200, 140}, {800, 480}, AnchorAlign::Left);
    CHECK(pos.x == 100);
    CHECK(pos.y == 144);
}

TEST_CASE("compute_card_pos: card flips above an anchor near the bottom",
          "[context-menu][position]") {
    // Below would be y=444, and 444+60 overruns the 470px usable height.
    const lv_point_t pos = below({80, 60}, {100, 400, 200, 440}, {800, 480});
    CHECK(pos.y == 336); // anchor top - card height - gap
}

TEST_CASE("compute_card_pos: a card that fits below is not flipped", "[context-menu][position]") {
    // Exactly flush with the usable bottom edge - the boundary case either side of
    // which the flip decision inverts.
    const lv_point_t pos = below({80, 60}, {100, 100, 200, 406}, {800, 480});
    CHECK(pos.y == 410);
    CHECK(pos.y + 60 == 470);
}

TEST_CASE("compute_card_pos: centring off the left edge clamps to the margin",
          "[context-menu][position]") {
    // Anchor hugs the left edge, so a centred card would start at -30.
    const lv_point_t pos = below({80, 60}, {0, 100, 20, 140}, {800, 480});
    CHECK(pos.x == kMargin);
}

TEST_CASE("compute_card_pos: centring off the right edge clamps to the margin",
          "[context-menu][position]") {
    const lv_point_t pos = below({80, 60}, {760, 100, 800, 140}, {800, 480});
    CHECK(pos.x == 800 - 80 - kMargin);
}

// ============================================================================
// ClickPoint
// ============================================================================

TEST_CASE("compute_card_pos: click-anchored card hangs just below the pointer",
          "[context-menu][position]") {
    const lv_point_t pos = at_click({80, 60}, 100, 100, {800, 480});
    CHECK(pos.x == 90);
    CHECK(pos.y == 105); // one past the point, plus the gap
}

TEST_CASE("compute_card_pos: click-anchored card mirrors about a right-edge click",
          "[context-menu][position]") {
    // 780-10=770 would put the card's right edge at 850, past the 790 usable width,
    // so it mirrors to the other side of the pointer instead of merely sliding.
    const lv_point_t pos = at_click({80, 60}, 780, 100, {800, 480});
    CHECK(pos.x == 710);
}

TEST_CASE("compute_card_pos: click-anchored card flips above a pointer near the bottom",
          "[context-menu][position]") {
    // Sliding up would cover the point the caret is meant to name.
    ContextMenu::AnchorSide side = ContextMenu::AnchorSide::Over;
    const lv_point_t pos =
        ContextMenu::compute_card_pos({80, 60}, {100, 440, 100, 440}, {800, 480}, kMargin, kGap,
                                      AnchorMode::ClickPoint, AnchorAlign::Center, &side);
    CHECK(pos.y == 440 - kGap - 60);
    CHECK(side == ContextMenu::AnchorSide::Above);
}

// ============================================================================
// Oversized cards - the clamp order
// ============================================================================

TEST_CASE("compute_card_pos: a card taller than the screen keeps its top on screen",
          "[context-menu][position]") {
    // The high-edge clamp alone yields 480-500-10 = -30, which would put the card's
    // header and first row above the top of the screen where they cannot be reached.
    const lv_point_t pos = below({80, 500}, {100, 100, 200, 140}, {800, 480});
    CHECK(pos.y == kMargin);
}

TEST_CASE("compute_card_pos: a card wider than the screen keeps its left edge on screen",
          "[context-menu][position]") {
    const lv_point_t pos = below({900, 60}, {100, 100, 200, 140}, {800, 480});
    CHECK(pos.x == kMargin);
}

TEST_CASE("compute_card_pos: an oversized click-anchored card clamps the same way",
          "[context-menu][position]") {
    const lv_point_t pos = at_click({900, 500}, 400, 240, {800, 480});
    CHECK(pos.x == kMargin);
    CHECK(pos.y == kMargin);
}

// ============================================================================
// AroundAnchor
// ============================================================================

namespace {

using AnchorSide = ContextMenu::AnchorSide;

struct Placed {
    lv_point_t pos;
    AnchorSide side;
};

Placed around(lv_point_t card, lv_area_t anchor, lv_point_t bounds, int32_t margin, int32_t gap) {
    AnchorSide side = AnchorSide::Over;
    const lv_point_t pos = ContextMenu::compute_card_pos(
        card, anchor, bounds, margin, gap, AnchorMode::AroundAnchor, AnchorAlign::Center, &side);
    return {pos, side};
}

/// A tile at (x, y), w by h, as inclusive LVGL coordinates.
lv_area_t tile(int32_t x, int32_t y, int32_t w, int32_t h) {
    return {x, y, x + w - 1, y + h - 1};
}

// The home grid's breakpoints: Medium 800x480 (card 180x138, margin 10, gap
// 5) and Micro 480x272 (card 120x84, margin 6, gap 2).
constexpr lv_point_t kMedium{800, 480};
constexpr lv_point_t kMediumCard{180, 138};
constexpr lv_point_t kMicro{480, 272};
constexpr lv_point_t kMicroCard{120, 84};

} // namespace

TEST_CASE("compute_card_pos: an around-anchored card goes below its tile when it fits",
          "[context-menu][position]") {
    const Placed p = around(kMediumCard, tile(560, 7, 114, 113), kMedium, 10, 5);
    CHECK(p.side == AnchorSide::Below);
    CHECK(p.pos.x == 527);
    CHECK(p.pos.y == 125);
}

TEST_CASE("compute_card_pos: an around-anchored card goes above when below overflows",
          "[context-menu][position]") {
    const Placed p = around(kMicroCard, tile(46, 136, 286, 132), kMicro, 6, 2);
    CHECK(p.side == AnchorSide::Above);
    CHECK(p.pos.x == 129);
    CHECK(p.pos.y == 50);
}

TEST_CASE("compute_card_pos: an around-anchored card goes right, then left, top-aligned",
          "[context-menu][position]") {
    // A tall tile at the right edge: no room below, above or right.
    const Placed left = around(kMediumCard, tile(560, 125, 233, 348), kMedium, 10, 5);
    CHECK(left.side == AnchorSide::Left);
    CHECK(left.pos.x == 375);
    CHECK(left.pos.y == 125);
    // The same tile mirrored to the left edge: right comes first.
    const Placed right = around(kMediumCard, tile(7, 125, 233, 348), kMedium, 10, 5);
    CHECK(right.side == AnchorSide::Right);
    CHECK(right.pos.x == 245);
    CHECK(right.pos.y == 125);
}

TEST_CASE("compute_card_pos: an around-anchored card with no side to go to sits over the tile",
          "[context-menu][position]") {
    const Placed p = around(kMicroCard, tile(0, 0, 480, 272), kMicro, 6, 2);
    CHECK(p.side == AnchorSide::Over);
    CHECK(p.pos.x == 240 - 60);
    CHECK(p.pos.y == 136 - 42);
    CHECK(p.pos.x >= 6);
    CHECK(p.pos.y >= 6);
    CHECK(p.pos.x + kMicroCard.x <= 480 - 6);
    CHECK(p.pos.y + kMicroCard.y <= 272 - 6);
}

TEST_CASE("compute_card_pos: a corner tile's around-anchored card clamps along its side",
          "[context-menu][position]") {
    const Placed p = around(kMicroCard, tile(406, 4, 70, 64), kMicro, 6, 2);
    CHECK(p.side == AnchorSide::Below);
    CHECK(p.pos.x == 354);
    CHECK(p.pos.y == 70);
}

TEST_CASE("compute_card_pos: at 480x272 the tile callout stays on screen beside tiles at every "
          "edge and corner",
          "[context-menu][position][edit-callout]") {
    // The grid host at Micro spans x 42..479 beside the navbar. A 1x1 tile is
    // 70x64, a 2x2 142x132. The cards: 112x78 as measured with both rows, and
    // the 120x84 worst case. The gap is space_xs plus the caret, 2 + 5.
    constexpr int32_t kMargin = 6;
    constexpr int32_t kGap = 7;
    constexpr int32_t kLeft = 46;
    constexpr int32_t kTop = 4;
    constexpr int32_t kRight = 476;
    constexpr int32_t kBottom = 268;
    const lv_point_t card = GENERATE(lv_point_t{112, 78}, lv_point_t{120, 84});
    const lv_point_t size = GENERATE(lv_point_t{70, 64}, lv_point_t{142, 132});
    const int32_t xs[] = {kLeft, (kLeft + kRight) / 2 - size.x / 2, kRight - size.x};
    const int32_t ys[] = {kTop, (kTop + kBottom) / 2 - size.y / 2, kBottom - size.y};
    for (int32_t x : xs) {
        for (int32_t y : ys) {
            const lv_area_t anchor = tile(x, y, size.x, size.y);
            const Placed p = around(card, anchor, kMicro, kMargin, kGap);
            const lv_area_t placed{p.pos.x, p.pos.y, p.pos.x + card.x - 1, p.pos.y + card.y - 1};
            CAPTURE(card.x, card.y, x, y, size.x, size.y, p.pos.x, p.pos.y,
                    static_cast<int>(p.side));
            CHECK(placed.x1 >= kMargin);
            CHECK(placed.y1 >= kMargin);
            CHECK(placed.x2 < kMicro.x - kMargin);
            CHECK(placed.y2 < kMicro.y - kMargin);
            // Every tile up to 2x2 leaves a side for the card: it never covers it.
            CHECK(p.side != AnchorSide::Over);
            CHECK((placed.x2 < anchor.x1 || placed.x1 > anchor.x2 || placed.y2 < anchor.y1 ||
                   placed.y1 > anchor.y2));
        }
    }
}

// ============================================================================
// side_of_anchor + compute_caret
// ============================================================================

namespace {

using CaretPlacement = ContextMenu::CaretPlacement;

constexpr int32_t kRadius = 8;
constexpr int32_t kCaret = 8;
// Closest the caret's centre may come to either end of an edge.
constexpr int32_t kInset = kRadius + kCaret;

/// A card at (x, y), w by h, placed against @p anchor; side read off the geometry.
CaretPlacement caret_for(lv_area_t card, lv_area_t anchor) {
    return ContextMenu::compute_caret(card, anchor, ContextMenu::side_of_anchor(card, anchor),
                                      kRadius, kCaret);
}

} // namespace

TEST_CASE("side_of_anchor: reads the side off the two rectangles", "[context-menu][caret]") {
    const lv_area_t anchor = tile(100, 100, 50, 50); // 100..149
    CHECK(ContextMenu::side_of_anchor(tile(80, 150, 90, 40), anchor) == AnchorSide::Below);
    CHECK(ContextMenu::side_of_anchor(tile(80, 59, 90, 41), anchor) == AnchorSide::Above);
    CHECK(ContextMenu::side_of_anchor(tile(150, 90, 60, 80), anchor) == AnchorSide::Right);
    CHECK(ContextMenu::side_of_anchor(tile(40, 90, 60, 80), anchor) == AnchorSide::Left);
    // Touching on one row overlaps: a card sharing the anchor's last row covers it.
    CHECK(ContextMenu::side_of_anchor(tile(80, 149, 90, 40), anchor) == AnchorSide::Over);
    CHECK(ContextMenu::side_of_anchor(tile(110, 110, 20, 20), anchor) == AnchorSide::Over);
    // Right of a point, then clamped down so it is below the point too: the
    // side it was placed on wins, and a preference it does not clear is ignored.
    const lv_area_t point{235, 152, 235, 152};
    const lv_area_t card = tile(250, 163, 350, 307);
    CHECK(ContextMenu::side_of_anchor(card, point) == AnchorSide::Below);
    CHECK(ContextMenu::side_of_anchor(card, point, AnchorSide::Right) == AnchorSide::Right);
    // A preference the card does not clear falls back to the geometry.
    CHECK(ContextMenu::side_of_anchor(card, point, AnchorSide::Left) == AnchorSide::Below);
}

TEST_CASE("compute_card_pos: a click-anchored card too tall for above or below goes beside the "
          "point",
          "[context-menu][caret]") {
    // The AMS slot menu's 353x307 card, raised from a tap at y 200.
    const lv_area_t point{235, 200, 235, 200};
    const lv_point_t card{353, 307};
    AnchorSide side = AnchorSide::Over;
    const lv_point_t pos = ContextMenu::compute_card_pos(
        card, point, {800, 480}, kMargin, kGap, AnchorMode::ClickPoint, AnchorAlign::Center, &side);
    CHECK(side == AnchorSide::Right);
    CHECK(pos.x == 236 + kGap);
    CHECK(pos.y == 200 - 307 / 2); // centred on the point
    const CaretPlacement p = ContextMenu::compute_caret(
        {pos.x, pos.y, pos.x + card.x - 1, pos.y + card.y - 1}, point, side, kRadius, kCaret);
    CHECK(p.side == AnchorSide::Right);
    CHECK(pos.y + p.offset == 200);
    // Near the right edge it goes left of the point instead.
    const lv_area_t far{700, 300, 700, 300};
    const lv_point_t left = ContextMenu::compute_card_pos(
        card, far, {800, 480}, kMargin, kGap, AnchorMode::ClickPoint, AnchorAlign::Center, &side);
    CHECK(side == AnchorSide::Left);
    CHECK(left.x + card.x - 1 == 700 - kGap - 1);
}

TEST_CASE("compute_caret: centred on the anchor below and above it", "[context-menu][caret]") {
    // Anchor midpoint x = (300 + 359) / 2 = 329; the card starts at 250.
    const lv_area_t anchor = tile(300, 200, 60, 40);
    const CaretPlacement below = caret_for(tile(250, 260, 200, 100), anchor);
    CHECK(below.side == AnchorSide::Below);
    CHECK(below.offset == 79);
    const CaretPlacement above = caret_for(tile(250, 80, 200, 100), anchor);
    CHECK(above.side == AnchorSide::Above);
    CHECK(above.offset == 79);
}

TEST_CASE("compute_caret: clamps clear of the card's corners", "[context-menu][caret]") {
    // A small anchor near the card's left end, then near its right end.
    const CaretPlacement left = caret_for(tile(100, 100, 200, 80), tile(98, 60, 6, 20));
    CHECK(left.side == AnchorSide::Below);
    CHECK(left.offset == kInset);
    const CaretPlacement right = caret_for(tile(100, 100, 200, 80), tile(296, 60, 6, 20));
    CHECK(right.side == AnchorSide::Below);
    CHECK(right.offset == 200 - 1 - kInset);
    // An anchor wholly beyond the card's span (the card was clamped to the
    // screen edge): the caret stays on the facing edge, at the nearer end.
    const CaretPlacement p = caret_for(tile(400, 100, 200, 80), tile(700, 20, 60, 40));
    CHECK(p.side == AnchorSide::Below);
    CHECK(p.offset == 200 - 1 - kInset);
    const CaretPlacement q = caret_for(tile(400, 100, 200, 80), tile(10, 300, 60, 40));
    CHECK(q.side == AnchorSide::Above);
    CHECK(q.offset == kInset);
}

TEST_CASE("compute_caret: on the left and right edges it follows the anchor's vertical middle",
          "[context-menu][caret]") {
    // Anchor rows 125..472 (a tall tile), midpoint 298; cards top-aligned at 125.
    const lv_area_t anchor = tile(560, 125, 233, 348);
    const CaretPlacement left = caret_for(tile(375, 125, 180, 138), anchor);
    CHECK(left.side == AnchorSide::Left);
    // 298 - 125 = 173 runs past the card's 138 rows: clamped to the bottom end.
    CHECK(left.offset == 138 - 1 - kInset);
    const CaretPlacement right = caret_for(tile(10, 200, 180, 300), tile(0, 250, 5, 21));
    CHECK(right.side == AnchorSide::Right);
    CHECK(right.offset == 260 - 200);
}

TEST_CASE("compute_caret: none over the anchor, the middle of a short edge",
          "[context-menu][caret]") {
    const CaretPlacement p = caret_for(tile(100, 100, 200, 100), tile(150, 120, 20, 20));
    CHECK(p.side == AnchorSide::Over);
    CHECK_FALSE(p.visible());
    CHECK_FALSE(ContextMenu::compute_caret(tile(0, 0, 10, 10), tile(0, 20, 10, 10),
                                           AnchorSide::Above, kRadius, 0)
                    .visible());
    // An edge too short for the inset gets the caret at its middle.
    CHECK(caret_for(tile(100, 100, 24, 80), tile(90, 60, 10, 10)).offset == 12);
}
