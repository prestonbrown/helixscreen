// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace helix::ui {

/**
 * @brief Share of the stacked column portrait reserves for the preview.
 *
 * The gcode viewer / thumbnail is why this screen exists, so in a stacked
 * portrait layout it gets a floor rather than whatever the controls leave over.
 * One number drives both consumers below, so "how much of a portrait
 * print-status screen is preview" is a single decision with a single place to
 * change it.
 */
inline constexpr int32_t PORTRAIT_PREVIEW_RESERVE_PCT = 40;

/**
 * @brief Vertical space the fan row may occupy, in px. Negative means no room.
 *
 * The landscape and portrait layouts are not two tunings of one rule; they are
 * different rules, because the container means different things.
 *
 * LANDSCAPE — `overlay_content` is a flex ROW and `controls_section` is
 * `height="100%"`. The column is a fixed budget and its children compete inside
 * it, so the budget is the column's own height minus what the children take.
 *
 * PORTRAIT — `overlay_content` is a flex COLUMN and `controls_section` is
 * `height="content"`. It has *no slack by construction*: its height is by
 * definition the sum of its children, so `controls_h - used` is approximately
 * zero (in fact slightly negative once the prospective fan row's gap is counted)
 * no matter how much screen is free. Feeding the landscape formula a portrait
 * layout therefore latches the fan row off permanently — the bug this exists to
 * prevent. The real competitor in portrait is the *preview*, which sits above
 * the controls with `flex_grow="1"` and absorbs whatever is left, so the budget
 * is the whole stacked column minus the preview's reserved floor.
 *
 * @param portrait   True when overlay_content is stacked (any portrait class).
 * @param controls_h Measured height of `controls_section`. Landscape only.
 * @param content_h  Measured height of `overlay_content`. Portrait only.
 * @param used       Summed heights of the visible controls children, including
 *                   inter-child gaps and one extra gap for the prospective fan
 *                   row.
 */
inline constexpr int32_t fan_row_budget(bool portrait, int32_t controls_h, int32_t content_h,
                                        int32_t used) {
    if (!portrait) {
        return controls_h - used;
    }
    const int32_t preview_floor = content_h * PORTRAIT_PREVIEW_RESERVE_PCT / 100;
    return content_h - preview_floor - used;
}

/**
 * @brief How tall the portrait preview art band may get, as a % of its own width.
 *
 * `thumbnail_section` is the only `flex_grow` child of the stacked portrait
 * column, so on an ultratall panel it absorbs every pixel the controls decline:
 * at 320x1480 that is a 302x1090 art band with the model adrift in the middle
 * of it. A preview is a picture of an object on a bed; past roughly
 * square-and-a-bit-taller it stops reading as one and starts reading as a
 * rendering failure.
 *
 * 130 (the band may be at most 1.30x its own width) was picked analytically —
 * it is the tallest band that still frames a typical bed-plus-headroom volume
 * without obvious letterboxing — NOT fitted to any one screenshot. It is
 * deliberately the ONLY place the number appears, so retuning the whole
 * behaviour is a one-line edit here.
 */
inline constexpr int32_t MAX_PREVIEW_ASPECT_PCT = 130;

/**
 * @brief Max height for portrait `thumbnail_section`, or 0 for "cannot decide".
 *
 * The cap belongs on the CARD, not on the inner `preview_clear_area`: capping
 * the band alone would leave the card itself still growing, and the slack would
 * surface as more card below the metadata strip — the exact look this exists to
 * remove. So the card's ceiling is the band's ceiling plus whatever the strip
 * currently needs.
 *
 * The strip is measured rather than assumed because it has two stable heights —
 * it gains a `text_small` line the moment Klipper publishes an M117.
 *
 * @param band_w   Measured CONTENT width of `thumbnail_section` — the art band's
 *                 own width. Not the border box: the card carries a 1px border,
 *                 and capping the box would cap a band 2px wider than exists.
 * @param chrome_h Everything in the card that is not the art band, in px: the
 *                 in-flow `metadata_clip` strip plus the card's own vertical
 *                 border/padding. Measured, not assumed.
 * @return Ceiling in px for the card's BOX height, or 0 when the width is not
 *         yet measurable — callers must treat 0 as "leave the layout alone",
 *         never as "clamp to nothing".
 */
inline constexpr int32_t portrait_preview_card_max_height(int32_t band_w, int32_t chrome_h) {
    if (band_w <= 0) {
        return 0;
    }
    return band_w * MAX_PREVIEW_ASPECT_PCT / 100 + (chrome_h > 0 ? chrome_h : 0);
}

/**
 * @brief Height for the slack absorber between the preview card and the controls.
 *
 * Where the leftover goes is the whole point. LVGL's flex places a track as
 * START whenever any grow item is present (`lv_flex.c`: `track_main_size =
 * grow_item_cnt ? max_main_size : ...`), so a capped card alone would strand the
 * remainder BELOW the controls, and `space_between` on the parent cannot reach
 * it either — the track always believes it is full. A sized sibling between the
 * two blocks is the only placement LVGL will honour.
 *
 * The absorber is a fixed-size (non-grow) child, so it costs one extra flex gap
 * when visible; that gap is charged here so the card lands on exactly `max_h`.
 * Returning 0 means "keep the absorber hidden", and a hidden child is skipped by
 * the flex pass entirely — size AND gap — which is what makes the uncapped sizes
 * bit-for-bit what they were before the cap existed.
 *
 * @param max_h  Ceiling from portrait_preview_card_max_height(); 0 disables.
 * @param avail_h Main-axis space the card and the absorber share, gap included.
 * @param gap    The column's `pad_gap`, spent on the absorber's own gap.
 */
inline constexpr int32_t portrait_preview_slack(int32_t max_h, int32_t avail_h, int32_t gap) {
    if (max_h <= 0) {
        return 0;
    }
    const int32_t slack = avail_h - max_h - gap;
    return slack > 0 ? slack : 0;
}

/**
 * @brief Shortest slack band worth putting a temperature graph in, in px.
 *
 * The absorber exists because the leftover has to go somewhere; a graph is what
 * makes the leftover worth having. But a chart squeezed under roughly this height
 * is a smear, not a reading — the plot area after the widget's own padding stops
 * resolving two traces apart, and a graph you cannot read is worse than the plain
 * band it replaced, because it costs a redraw every sample.
 *
 * Deliberately a floor on the SLACK, not a screen-size list: the slack is already
 * the measured outcome of width, chrome and control height, so anything that
 * changes the layout is accounted for without enumerating resolutions.
 */
inline constexpr int32_t MIN_TEMP_GRAPH_HEIGHT_PX = 120;

/**
 * @brief Extra slack the graph must gain before it comes BACK, in px.
 *
 * Same job as the +4 in recompute_fans_fit(): showing the graph does not change
 * the slack (it is a 100%-sized child of the absorber, not a flex sibling), but
 * the slack itself is recomputed from measurements that move by a pixel or two
 * as the metadata strip gains and loses its M117 line. Without a dead band a
 * layout parked exactly on the threshold would toggle the graph on every
 * recompute. Asymmetric on purpose — cheap to keep showing, dearer to start.
 */
inline constexpr int32_t TEMP_GRAPH_FIT_HYSTERESIS_PX = 8;

/**
 * @brief Does the temperature mini-graph fit in the portrait slack band?
 *
 * @param slack_h Current absorber height from portrait_preview_slack(); 0 in
 *                landscape and at every size where the aspect cap does not bind.
 * @param shown   Whether the graph is visible right now — the hysteresis input.
 *                Callers pass the CURRENT subject value, so the answer is a
 *                function of state, not a pure threshold.
 */
inline constexpr bool portrait_graph_fits(int32_t slack_h, bool shown) {
    return shown ? slack_h >= MIN_TEMP_GRAPH_HEIGHT_PX
                 : slack_h >= MIN_TEMP_GRAPH_HEIGHT_PX + TEMP_GRAPH_FIT_HYSTERESIS_PX;
}

/**
 * @brief How tall the mini-graph may get, as a % of its own width.
 *
 * Filling the whole absorber was the first attempt and it read badly. At
 * 320x1480 the slack is ~709px against a ~302px width: a reheat ramp becomes a
 * vertical wall, the top quarter of the plot never holds a sample, and once the
 * startup transient scrolls off the whole thing is three flat lines suspended in
 * half a screen of void.
 *
 * 100 — the graph may be at most 1.00x its own width, i.e. square. At ~300px
 * wide a time-series plot cannot be made WIDER than tall, so square is the
 * practical ceiling; past it the value axis is stretched against a time axis
 * that is already compressed, which exaggerates every ramp and flattens nothing
 * back out.
 *
 * Deliberately the ONLY place the number appears, so retuning the graph's shape
 * is a one-line edit here.
 */
inline constexpr int32_t MAX_GRAPH_ASPECT_PCT = 100;

/**
 * @brief Height for the mini-graph inside the slack absorber, 0 = cannot decide.
 *
 * The absorber keeps the WHOLE slack — it is what holds the preview card down to
 * its ceiling and the controls against the screen bottom, so shrinking it would
 * hand the space straight back to the card. Only the graph is capped; the
 * remainder of the absorber stays transparent and reads as background between
 * the graph and the controls, which is where the leftover was always going to
 * land.
 *
 * Independent of portrait_graph_fits(): "does a graph belong here at all" is a
 * floor on the slack (MIN_TEMP_GRAPH_HEIGHT_PX), while this is a ceiling on the
 * graph. A graph that fits but is capped is the normal ultratall case; a slack
 * under the floor still means hidden, and this returns a height nobody reads.
 *
 * @param graph_w Measured width of the graph container. <= 0 means the subtree
 *                is not laid out yet — callers must treat 0 as "leave the height
 *                alone", never as "collapse it".
 * @param slack_h Absorber height from portrait_preview_slack(). The graph never
 *                exceeds it, so on a short band the cap simply does not bind.
 */
inline constexpr int32_t portrait_graph_height(int32_t graph_w, int32_t slack_h) {
    if (graph_w <= 0 || slack_h <= 0) {
        return 0;
    }
    const int32_t cap = graph_w * MAX_GRAPH_ASPECT_PCT / 100;
    return slack_h < cap ? slack_h : cap;
}

/**
 * @brief Where the exclude-object side list sits over the print-status content.
 *
 * The list is a FLOATING child of `overlay_content`, so it overlays rather than
 * displaces. Its job is to cover the controls while the object map takes the
 * preview — which is a different edge in each layout.
 *
 * LANDSCAPE — controls are the right-hand column, historically 4/9 of the row,
 * so a 44%-wide list anchored right covers them almost exactly.
 *
 * PORTRAIT — controls are the bottom of a stack, so a 44%-wide list anchored
 * right covers the right 44% of *everything* and none of the controls fully.
 * The list goes full width, anchored to the bottom, and is sized in px from the
 * MEASURED control stack. It deliberately does not read PORTRAIT_PREVIEW_RESERVE_PCT:
 * that constant answers "how much of the column is preview", which says nothing
 * about how tall a list of objects should be, and borrowing it once already cost
 * the map 40%+ of its tappable area when the controls shrank underneath it.
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
inline constexpr int32_t MIN_SIDE_LIST_HEIGHT_PX = 160;

/**
 * @brief Most of the stacked column the list may ever cover, in %.
 *
 * The list accompanies the object map; it must never become the reason the map
 * cannot be tapped. This is a backstop on pathological control stacks, not the
 * normal sizing path — in portrait the controls come in far under it.
 */
inline constexpr int32_t MAX_SIDE_LIST_COVERAGE_PCT = 55;

/**
 * @brief Height used when the portrait column has not been measured yet, in %.
 *
 * Deliberately NOT derived from PORTRAIT_PREVIEW_RESERVE_PCT. Nothing about "how
 * much of the screen is preview" tells you how tall a list of objects is; the
 * old coupling is exactly the bug this file now avoids. A caller that reaches
 * this has no measurements at all, so any number is a guess — this one is just
 * a middling guess that leaves the map visible.
 */
inline constexpr int32_t PORTRAIT_SIDE_LIST_FALLBACK_PCT = 50;

/**
 * @brief Portrait side-list height in px, or 0 when nothing is measurable yet.
 *
 * The list's job is to cover the controls, so it is sized from the CONTROLS —
 * measured, every time — rather than from a share of the column. A percentage
 * cannot notice that the control stack shrank, which is how a rule written when
 * the controls were ~55% survived them falling to 14-34% and started eating a
 * live tappable object map instead.
 *
 * @param controls_h Measured height of `controls_section`. <= 0 means unmeasured.
 * @param content_h  Measured CONTENT height of `overlay_content` — what a
 *                   percentage would have resolved against. <= 0 means unmeasured.
 * @param gap        The column's `pad_gap`, so the list clears the control stack
 *                   by the same rhythm the rest of the column uses.
 * @return Height in px, clamped to [floor, ceiling]; 0 when unmeasurable, which
 *         callers must treat as "fall back to height_pct", never as "collapse".
 */
inline constexpr int32_t portrait_side_list_height(int32_t controls_h, int32_t content_h,
                                                   int32_t gap) {
    if (controls_h <= 0 || content_h <= 0) {
        return 0;
    }
    const int32_t ceiling = content_h * MAX_SIDE_LIST_COVERAGE_PCT / 100;
    // A column too short to honour the floor gets the ceiling: covering the map
    // is still better than a list with nothing in it.
    const int32_t floor_h = MIN_SIDE_LIST_HEIGHT_PX < ceiling ? MIN_SIDE_LIST_HEIGHT_PX : ceiling;
    const int32_t want = controls_h + (gap > 0 ? gap : 0);
    if (want < floor_h) {
        return floor_h;
    }
    return want > ceiling ? ceiling : want;
}

/**
 * @param portrait   True when overlay_content is stacked.
 * @param controls_h Measured `controls_section` height. Portrait only; ignored
 *                   in landscape, where the 44%/100% rule is already exact.
 * @param content_h  Measured `overlay_content` content height. Portrait only.
 * @param gap        The column's `pad_gap`. Portrait only.
 */
inline constexpr SideListGeometry exclude_side_list_geometry(bool portrait, int32_t controls_h = 0,
                                                             int32_t content_h = 0,
                                                             int32_t gap = 0) {
    if (!portrait) {
        // 44% ~= 4/9, the controls column's share of the landscape row — it is
        // flex_grow="4" against thumbnail_section's flex_grow="5", so this is
        // derived, not tuned, and needs no measurement.
        return SideListGeometry{44, 100, 0, false};
    }
    return SideListGeometry{100, PORTRAIT_SIDE_LIST_FALLBACK_PCT,
                            portrait_side_list_height(controls_h, content_h, gap), true};
}

} // namespace helix::ui
