// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "card_thumbnail_plan.h"

#include <algorithm>

namespace helix {

CardThumbnailPlan plan_card_thumbnails(const std::vector<CardThumbnailState>& files, size_t first,
                                       size_t end, size_t in_flight, size_t estimate, size_t budget,
                                       bool lane_refused, bool keep_off_screen, size_t max_new) {
    CardThumbnailPlan plan;
    end = std::min(end, files.size());
    first = std::min(first, end);
    // A thumbnail fills a whole card-sized slot, however small its image.
    const auto slot = [estimate](const CardThumbnailState& f) {
        return std::max(f.held, estimate);
    };

    size_t committed = in_flight * estimate;
    std::vector<size_t> kept; // off screen and holding: kept while the budget has room
    for (size_t i = 0; i < files.size(); ++i) {
        const CardThumbnailState& f = files[i];
        if (i >= first && i < end) {
            committed += f.held ? slot(f) : 0;
        } else if (f.held) {
            kept.push_back(i);
        } else if (f.tried) {
            plan.drop.push_back(i);
        }
    }

    // Whole cards first: one cut by an edge is the likelier to scroll away
    // before its fetch reaches the front of the lane.
    for (const bool whole : {true, false}) {
        for (size_t i = first; i < end && !lane_refused; ++i) {
            const CardThumbnailState& f = files[i];
            if (f.whole != whole || !f.fetchable || f.tried || f.held) {
                continue;
            }
            if (committed > budget || budget - committed < estimate) {
                break;
            }
            if (plan.fetch.size() >= max_new) {
                plan.capped = true;
                break;
            }
            committed += estimate;
            plan.fetch.push_back(i);
        }
    }

    // Cards on screen and fetches come first; the most recently shown cards
    // off screen keep what is left, so scrolling back redraws without a decode.
    std::stable_sort(kept.begin(), kept.end(), [&files](size_t a, size_t b) {
        return files[a].last_shown > files[b].last_shown;
    });
    for (size_t i : kept) {
        if (keep_off_screen && committed <= budget && budget - committed >= slot(files[i])) {
            committed += slot(files[i]);
        } else {
            plan.drop.push_back(i);
        }
    }
    std::sort(plan.drop.begin(), plan.drop.end());
    return plan;
}

} // namespace helix
