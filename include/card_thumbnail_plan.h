// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Which print-file cards hold a decoded thumbnail, decided without the panel:
// cards in the visible window come first, cards that left it keep theirs in
// least-recently-shown order while room is left, and everything held or in
// flight stays within a byte budget. The panel applies the plan.

#include "thumbnail_downscale.h" // ThumbnailDecodeFailure

#include <cstddef>
#include <cstdint>
#include <vector>

namespace helix {

/// One file as the planner sees it.
struct CardThumbnailState {
    bool fetchable = false;  ///< a file (not a directory) with a thumbnail to fetch
    bool tried = false;      ///< a fetch was started while its card was on screen
    size_t held = 0;         ///< bytes its decoded thumbnail occupies, 0 when it holds none
    uint32_t last_shown = 0; ///< when its card was last on screen; larger is more recent
};

struct CardThumbnailPlan {
    std::vector<size_t> drop;  ///< off-screen files to release: thumbnail and tried mark, ascending
    std::vector<size_t> fetch; ///< files to start fetching now, in this order
};

/**
 * @brief Plans thumbnails for the card window [first, end).
 *
 * Inside the window, files that are fetchable, untried and hold nothing are
 * fetched in order while the window's held thumbnails, the fetches already in
 * flight and the ones planned fit @p budget; the rest wait for a later pass.
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
                                       bool lane_refused = false, bool keep_off_screen = true);

/// A card thumbnail decode as it comes back to the panel.
struct CardThumbnailResult {
    bool decoded = false;
    ThumbnailDecodeFailure failure = ThumbnailDecodeFailure::BadImage;
    bool shown = true;    ///< its card is still on screen with its fetch marked
    bool current = true;  ///< decoded into the pool and backdrop the panel uses now
    bool retried = false; ///< it already ran out of memory once this showing
};

enum class CardThumbnailOutcome {
    Keep,    ///< show it
    Retry,   ///< fetch it again after the lane pause
    Discard, ///< drop it; the card keeps its placeholder until next shown
};

/// What the panel does with a finished decode. Memory frees up as other cards
/// go, so running out is retried, once per showing so a PSRAM that stays tight
/// cannot refetch every card on screen over and over; a bad image stays bad.
/// A result for a card that left, or made for a pool or backdrop since
/// replaced, is neither kept nor retried.
inline CardThumbnailOutcome card_thumbnail_outcome(const CardThumbnailResult& r) {
    if (!r.shown || !r.current) {
        return CardThumbnailOutcome::Discard;
    }
    if (r.decoded) {
        return CardThumbnailOutcome::Keep;
    }
    return r.failure == ThumbnailDecodeFailure::OutOfMemory && !r.retried
               ? CardThumbnailOutcome::Retry
               : CardThumbnailOutcome::Discard;
}

} // namespace helix
