// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace helix::ui {

/**
 * @brief Where the exclude-object side list sits over the print-status content.
 *
 * The list is a FLOATING child of `overlay_content`, so it overlays rather than
 * displaces. Its job is to cover the controls while the object map takes the
 * preview — which is a different edge in each layout.
 *
 * LANDSCAPE — controls are the right-hand column, 4/9 of the row,
 * so a 44%-wide list anchored right covers them almost exactly.
 *
 * PORTRAIT — controls are the bottom of a stack, so a 44%-wide list anchored
 * right covers the right 44% of *everything* and none of the controls fully.
 * The list goes full width, anchored to the bottom, and is sized in px from the
 * MEASURED control stack: how much of the column is preview says nothing about
 * how tall a list of objects should be.
 */
struct SideListGeometry {
    int32_t width_pct;
    int32_t height_pct; ///< Used only when height_px == 0.
    int32_t height_px;  ///< Measured height; 0 = "not measurable, use height_pct".
    bool anchor_bottom; ///< false = right edge (landscape), true = bottom (portrait)
};

/**
 * @brief Shortest side list worth sliding in, in px.
 *
 * Below roughly this the header row plus one object row no longer both fit, and
 * a list that cannot show a single tappable entry is worse than no list — the
 * user gave up the map for nothing.
 */
inline constexpr int32_t kMinSideListHeightPx = 160;

/**
 * @brief Most of the stacked column the list may ever cover, in %, when the
 * room below the preview card is not measured.
 *
 * The list accompanies the object map; it must never become the reason the map
 * cannot be tapped. This is a backstop on pathological control stacks, not the
 * normal sizing path — in portrait the controls come in far under it.
 */
inline constexpr int32_t kMaxSideListCoveragePct = 55;

/**
 * @brief Height used when the portrait column has not been measured yet, in %.
 *
 * Nothing about how much of the screen is preview tells you how tall a list of
 * objects is. A caller that reaches this has no measurements at all, so any
 * number is a guess; this one is a middling guess that leaves the map visible.
 */
inline constexpr int32_t kPortraitSideListFallbackPct = 50;

/**
 * @brief Portrait side-list height in px, or 0 when nothing is measurable yet.
 *
 * The list's job is to cover the controls, so it is sized from the CONTROLS —
 * measured, every time — rather than from a share of the column. A percentage
 * cannot notice that the control stack shrank, which is how a rule written when
 * the controls were ~55% survived them falling to 14-34% and started eating a
 * live tappable object map instead.
 *
 * @param controls_h Measured height of the controls the list covers. <= 0 means unmeasured.
 * @param content_h  Measured CONTENT height of `overlay_content` — what a
 *                   percentage would have resolved against. <= 0 means unmeasured.
 * @param gap        The column's `pad_gap`, so the list clears the control stack
 *                   by the same rhythm the rest of the column uses.
 * @param room_h     Measured room between the preview card and the column's
 *                   bottom, less one gap. When > 0 it is the ceiling, so the list
 *                   covers every control below the card and never the card;
 *                   <= 0 falls back to kMaxSideListCoveragePct.
 * @return Height in px, clamped to [floor, ceiling]; 0 when unmeasurable, which
 *         callers must treat as "fall back to height_pct", never as "collapse".
 */
inline constexpr int32_t portrait_side_list_height(int32_t controls_h, int32_t content_h,
                                                   int32_t gap, int32_t room_h = 0) {
    if (controls_h <= 0 || content_h <= 0) {
        return 0;
    }
    const int32_t ceiling = room_h > 0 ? room_h : content_h * kMaxSideListCoveragePct / 100;
    // A column too short to honour the floor gets the ceiling: covering the map
    // is still better than a list with nothing in it.
    const int32_t floor_h = kMinSideListHeightPx < ceiling ? kMinSideListHeightPx : ceiling;
    const int32_t want = controls_h + (gap > 0 ? gap : 0);
    if (want < floor_h) {
        return floor_h;
    }
    return want > ceiling ? ceiling : want;
}

/**
 * @param portrait   True when overlay_content is stacked.
 * @param controls_h Measured height of the controls the list covers. Portrait only; ignored
 *                   in landscape, where the 44%/100% rule is already exact.
 * @param content_h  Measured `overlay_content` content height. Portrait only.
 * @param gap        The column's `pad_gap`. Portrait only.
 * @param room_h     Room below the preview card, less one gap. Portrait only.
 */
inline constexpr SideListGeometry exclude_side_list_geometry(bool portrait, int32_t controls_h = 0,
                                                             int32_t content_h = 0, int32_t gap = 0,
                                                             int32_t room_h = 0) {
    if (!portrait) {
        // 44% ~= 4/9, the options column's share of print details' landscape
        // row (flex_grow 4 against 5). Print status splits its tiles 1:1, so
        // there the list covers all but the inner edge of the printer tile.
        return SideListGeometry{44, 100, 0, false};
    }
    return SideListGeometry{100, kPortraitSideListFallbackPct,
                            portrait_side_list_height(controls_h, content_h, gap, room_h), true};
}

/**
 * @brief Do the action row's buttons fit with each icon beside its label?
 *
 * When they do not, the row stacks every icon over its label instead, which
 * needs only the wider of the two per button. Each width is what the button
 * needs laid out side by side (icon, gap, label text, side padding, border),
 * measured from its text rather than its current geometry, so the decision
 * does not depend on which way the row is laid out now.
 *
 * @param row_w   The row's content width.
 * @param gap     The row's gap between buttons.
 * @param needed  Side-by-side width of each visible button.
 * @param count   Number of entries in @p needed.
 */
inline constexpr bool action_row_fits(int32_t row_w, int32_t gap, const int32_t* needed,
                                      int32_t count) {
    int32_t total = count > 1 ? gap * (count - 1) : 0;
    for (int32_t i = 0; i < count; ++i) {
        total += needed[i];
    }
    return total <= row_w;
}

} // namespace helix::ui
