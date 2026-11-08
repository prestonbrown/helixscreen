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

TEST_CASE("compute_card_pos: click-anchored card hangs off the pointer",
          "[context-menu][position]") {
    const lv_point_t pos = at_click({80, 60}, 100, 100, {800, 480});
    CHECK(pos.x == 90);
    CHECK(pos.y == 90);
}

TEST_CASE("compute_card_pos: click-anchored card mirrors about a right-edge click",
          "[context-menu][position]") {
    // 780-10=770 would put the card's right edge at 850, past the 790 usable width,
    // so it mirrors to the other side of the pointer instead of merely sliding.
    const lv_point_t pos = at_click({80, 60}, 780, 100, {800, 480});
    CHECK(pos.x == 710);
}

TEST_CASE("compute_card_pos: click-anchored card slides up rather than flipping",
          "[context-menu][position]") {
    // Vertically it never flips - the pointer is on a row, and jumping the card to
    // the far side of it would leave the row it describes uncovered but far away.
    const lv_point_t pos = at_click({80, 60}, 100, 460, {800, 480});
    CHECK(pos.y == 480 - 60 - kMargin);
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
