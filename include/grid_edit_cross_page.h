// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

/**
 * @file grid_edit_cross_page.h
 * @brief Pure cross-page rules for a home-grid edit drag: edge push, majority
 * crossing, edge dwell, and whether a drop would create a page.
 *
 * Horizontal only, and free of LVGL: GridEditMode gathers the inputs from the
 * live drag and applies the step's result (prestonbrown/helixscreen#1638).
 */

namespace helix {

/// Delay between a majority crossing and the page flip it requests, in ms.
///
/// A crossing flips only if the finger is still down, with the widget still
/// past the line, when the delay runs out. Placing a widget in the outer column
/// easily carries it past the line, so the delay leaves time to settle it there
/// and lift: the release drops on the page the user sees. A widget held past
/// the line still flips in well under the dwell (#2070).
inline constexpr uint32_t kCrossPageCrossingDelayMs = 400;

/// How long the pointer stays in an edge zone before the dwell flips, in ms.
///
/// Long enough that carrying a widget through the zone to the outer column and
/// pausing there does not flip, short enough to read as a response to a held
/// push.
inline constexpr uint32_t kCrossPageDwellMs = 900;

/// How far past the majority line the edge push may carry a widget, in px.
///
/// The cap sits just past the line on both sides, so a pinned pointer always
/// reaches a crossing and never runs the widget further off the page than that.
inline constexpr int kCrossPagePushCapSlopPx = 4;

/// Width of the edge zone inside each side of the page frame, in px, for a grid
/// whose column track is @p cell_w px wide.
///
/// A share of the track rather than a fixed count, the way the resize grab band
/// is derived: 40px on the 800x480 panel and the same share of a track on every
/// other panel, with a floor that keeps a fingertip-wide zone on the smallest.
///
/// @return The 800x480 panel's zone when @p cell_w is not positive.
int cross_page_edge_zone_px(float cell_w);

/// Edge push speed in px per second for a column track of @p cell_w px.
///
/// Proportional to the track, so pushing a widget of N tracks across the border
/// takes the same time on every panel: about 180px/s on the 800x480 panel.
///
/// @return The 800x480 panel's speed when @p cell_w is not positive.
int cross_page_push_px_per_s(float cell_w);

/// Per-drag state cross_page_step() carries between moves. Value-initialize it
/// at gesture end; a default state is a drag that has not touched a zone.
struct CrossPageState {
    /// Accumulated edge push in thousandths of a px, signed (negative is
    /// leftward), so sub-pixel steps at any read cadence add up exactly.
    int push_mpx = 0;
    /// Edge zone the pointer was in at the previous step: -1 left, +1 right, 0 none.
    int zone_dir = 0;
    /// The current zone stay already produced a flip, so neither trigger fires
    /// again until the pointer leaves the zone.
    bool zone_spent = false;
    /// A crossing may request a flip. Cleared by a crossing, set again only
    /// once the widget is majority inside the frame.
    bool crossing_armed = true;
    /// The last crossing's flip is requested and has not happened yet.
    bool crossing_pending = false;
    /// That pending crossing is what spent the zone stay.
    bool crossing_spent_zone = false;
    /// Direction the dwell is counting toward: -1, +1, or 0 when no dwell runs.
    int dwell_dir = 0;
    /// Time the running dwell has counted, as of the previous step.
    uint32_t dwell_ms = 0;
};

/// What one drag move looks like, in screen coordinates.
struct CrossPageInput {
    int frame_x1 = 0;    ///< Left edge of the settled page content area
    int frame_x2 = 0;    ///< Right edge of the settled page content area (inclusive)
    float cell_w = 0.0f; ///< Column track width the zone and push speed derive from
    int pointer_x = 0;
    int widget_left = 0; ///< Pointer x minus the grab offset, before any push
    int widget_width = 0;
    uint32_t elapsed_ms = 0;         ///< Time since the previous step
    int page_index = 0;              ///< Page the session is scoped to
    int page_count = 0;              ///< Real pages in the config
    bool has_next_page_slot = false; ///< A page past the last one can be created
};

/// What the drag does with this move.
struct CrossPageStep {
    int widget_left = 0;          ///< Widget left edge after the edge push
    int flip_dir = 0;             ///< Crossing flip to request now: -1, +1, or 0 for none
    bool cancel_crossing = false; ///< Drop the pending crossing flip: the widget is back inside
    uint32_t flip_delay_ms = 0;   ///< When flip_dir is set: how long until the crossing flips
    int dwell_dir = 0;            ///< Direction the dwell counts toward: -1, +1, or 0 for none
    bool dwell_changed = false;   ///< dwell_dir differs from the previous step: restart or cancel
};

/// Whether a widget whose left edge is @p widget_left has its majority past the
/// page frame's right edge @p frame_x2 (inclusive).
bool cross_page_past_right_border(int frame_x2, int widget_left, int widget_width);

/// Whether a widget whose left edge is @p widget_left has its majority past the
/// page frame's left edge @p frame_x1: its right edge lies left of the majority
/// line.
bool cross_page_past_left_border(int frame_x1, int widget_left, int widget_width);

/// Whether a release creates a page: a page can be created
/// (@p has_next_page_slot), and the session is scoped to the page past the last
/// one, wherever the widget is, or to the last page with the widget's majority
/// past its right border, or to the first page with the widget's majority past
/// its left border, which creates a page before it. Asked at the release, with
/// the scope the release lands in.
bool cross_page_drop_creates_page(int page_index, int page_count, bool has_next_page_slot,
                                  bool past_right_border, bool past_left_border);

/**
 * @brief Advance the cross-page rules by one drag move.
 *
 * - Edge push: while the pointer stays in an edge zone the widget slides past
 *   where the pointer puts it, by the push speed times the time spent in the
 *   zone, capped kCrossPagePushCapSlopPx past the majority line on either
 *   side. Leaving or changing zones drops the widget back on the pointer.
 * - Crossing: the widget's majority beyond a frame edge requests one flip
 *   toward it; the next crossing waits until the widget is majority inside.
 *   The flip waits kCrossPageCrossingDelayMs, or less when it cuts off a dwell
 *   counting the same way: never later than that dwell would have flipped.
 *   Coming back inside before that flip happens cancels it and gives back the
 *   zone stay it spent, so the dwell counts again.
 * - Dwell: runs while the pointer is in a zone, restarting on zone entry and
 *   stopping on exit.
 * - One flip per zone stay: a crossing stops the dwell, and a flip spends the
 *   zone for both triggers until the pointer leaves it. A dwell and a crossing
 *   therefore never chain two pages from one held push.
 *
 * @param state Carried between moves of one drag.
 * @param in The move.
 * @return What the drag applies.
 */
CrossPageStep cross_page_step(CrossPageState& state, const CrossPageInput& in);

/// Record that the pending crossing's flip happened, so the widget coming back
/// inside afterwards cancels nothing.
void cross_page_note_crossing_flip(CrossPageState& state);

/// Record that the dwell's flip was requested: it spends the current zone stay,
/// so no crossing or dwell in that zone requests another flip until the pointer
/// leaves it.
void cross_page_note_dwell_flip(CrossPageState& state);

} // namespace helix
