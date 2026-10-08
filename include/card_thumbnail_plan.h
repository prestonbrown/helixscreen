// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Which print-file cards hold a decoded thumbnail, decided without the panel:
// cards in the visible window come first, cards that left it keep theirs in
// least-recently-shown order while room is left, and everything held or in
// flight stays within a byte budget. The panel applies the plan.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace helix {

/// One file as the planner sees it.
struct CardThumbnailState {
    bool fetchable = false;  ///< a file (not a directory) with a thumbnail to fetch
    bool tried = false;      ///< a fetch was started while its card was on screen
    size_t held = 0;         ///< bytes its decoded thumbnail occupies, 0 when it holds none
    uint32_t last_shown = 0; ///< when its card was last on screen; larger is more recent
    bool whole = false;      ///< its card is wholly on screen, not cut by an edge
};

struct CardThumbnailPlan {
    std::vector<size_t> drop;  ///< off-screen files to release: thumbnail and tried mark, ascending
    std::vector<size_t> fetch; ///< files to start fetching now, in this order
    bool capped = false;       ///< a file the budget had room for waits on @p max_new
};

/**
 * @brief Plans thumbnails for the card window [first, end).
 *
 * Inside the window, files that are fetchable, untried and hold nothing are
 * fetched while the window's held thumbnails, the fetches already in flight and
 * the ones planned fit @p budget, and while fewer than @p max_new are planned;
 * the rest wait for a later pass. Files whose card is whole on screen go first,
 * then the ones cut by an edge, each in index order.
 * Outside it, a file with a tried mark and nothing held is dropped, and files
 * holding a thumbnail keep it, most recently shown first, in whatever budget
 * the window and its fetches leave; the rest are dropped. A file on screen is
 * never dropped. Each counts at least @p estimate, the size of the slot it
 * decodes into, so held thumbnails, fetches in flight and planned fetches
 * together never take more than budget / estimate slots.
 *
 * With @p keep_off_screen false every file outside the window holding a
 * thumbnail is dropped, so only the window's slots stay held.
 *
 * @p lane_refused says the HTTP lane turned a fetch away and no slot has freed
 * since: nothing is fetched until one does, however often the window is
 * re-planned, since every fetch would be refused the same way.
 */
CardThumbnailPlan plan_card_thumbnails(const std::vector<CardThumbnailState>& files, size_t first,
                                       size_t end, size_t in_flight, size_t estimate, size_t budget,
                                       bool lane_refused = false, bool keep_off_screen = true,
                                       size_t max_new = std::numeric_limits<size_t>::max());

/// How many card fetches may start when the transport has @p free_slots
/// request slots left: all but @p reserve, which stay for other requests.
inline size_t card_thumbnail_fetch_room(size_t free_slots, size_t reserve) {
    return free_slots > reserve ? free_slots - reserve : 0;
}

/// Whether a card whose fetch failed (a stalled or timed-out download, an HTTP
/// error) is fetched again: once per showing, and only while it is shown and
/// was not cancelled for leaving the screen.
inline bool card_thumbnail_refetch_after_error(bool shown, bool cancelled, bool retried) {
    return shown && !cancelled && !retried;
}

} // namespace helix
