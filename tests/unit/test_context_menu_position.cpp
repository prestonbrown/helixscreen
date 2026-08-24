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

constexpr int32_t MARGIN = 10;
constexpr int32_t GAP = 4;

lv_point_t below(lv_point_t card, lv_area_t anchor, lv_point_t bounds,
                 AnchorAlign align = AnchorAlign::Center) {
    return ContextMenu::compute_card_pos(card, anchor, bounds, MARGIN, GAP, AnchorMode::BelowAnchor,
                                         align);
}

lv_point_t at_click(lv_point_t card, int32_t x, int32_t y, lv_point_t bounds) {
    return ContextMenu::compute_card_pos(card, {x, y, x, y}, bounds, MARGIN, GAP,
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
    CHECK(pos.x == MARGIN);
}

TEST_CASE("compute_card_pos: centring off the right edge clamps to the margin",
          "[context-menu][position]") {
    const lv_point_t pos = below({80, 60}, {760, 100, 800, 140}, {800, 480});
    CHECK(pos.x == 800 - 80 - MARGIN);
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
    CHECK(pos.y == 480 - 60 - MARGIN);
}

// ============================================================================
// Oversized cards - the clamp order
// ============================================================================

TEST_CASE("compute_card_pos: a card taller than the screen keeps its top on screen",
          "[context-menu][position]") {
    // The high-edge clamp alone yields 480-500-10 = -30, which would put the card's
    // header and first row above the top of the screen where they cannot be reached.
    const lv_point_t pos = below({80, 500}, {100, 100, 200, 140}, {800, 480});
    CHECK(pos.y == MARGIN);
}

TEST_CASE("compute_card_pos: a card wider than the screen keeps its left edge on screen",
          "[context-menu][position]") {
    const lv_point_t pos = below({900, 60}, {100, 100, 200, 140}, {800, 480});
    CHECK(pos.x == MARGIN);
}

TEST_CASE("compute_card_pos: an oversized click-anchored card clamps the same way",
          "[context-menu][position]") {
    const lv_point_t pos = at_click({900, 500}, 400, 240, {800, 480});
    CHECK(pos.x == MARGIN);
    CHECK(pos.y == MARGIN);
}
