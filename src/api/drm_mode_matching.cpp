// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/drm_mode_matching.h"

#include <cstddef>

namespace helix {

int find_matching_mode(const std::vector<DrmModeInfo>& modes, uint32_t requested_w,
                       uint32_t requested_h) {
    for (size_t i = 0; i < modes.size(); i++) {
        if (modes[i].hdisplay == requested_w && modes[i].vdisplay == requested_h) {
            return static_cast<int>(i);
        }
    }
    return DrmModeMatch::NO_MATCH;
}

int find_preferred_mode_index(const std::vector<DrmModeInfo>& modes) {
    if (modes.empty()) {
        return DrmModeMatch::NO_MATCH;
    }
    for (size_t i = 0; i < modes.size(); i++) {
        if (modes[i].is_preferred) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

int find_best_downscale_mode(const std::vector<DrmModeInfo>& modes, uint32_t max_axis) {
    if (modes.empty()) {
        return DrmModeMatch::NO_MATCH;
    }

    // Find the preferred mode to check if downscaling is needed.
    int pref_idx = find_preferred_mode_index(modes);
    if (pref_idx == DrmModeMatch::NO_MATCH) {
        return DrmModeMatch::NO_MATCH;
    }

    const auto& pref = modes[pref_idx];
    if (pref.hdisplay <= max_axis && pref.vdisplay <= max_axis) {
        return DrmModeMatch::NO_MATCH; // No downscaling needed
    }

    // Preferred exceeds threshold — find the best sub-threshold alternative.
    int best = DrmModeMatch::NO_MATCH;
    uint64_t best_pixels = 0;
    uint32_t best_refresh = 0;

    for (size_t i = 0; i < modes.size(); i++) {
        if (modes[i].hdisplay > max_axis || modes[i].vdisplay > max_axis) {
            continue;
        }
        uint64_t pixels = static_cast<uint64_t>(modes[i].hdisplay) * modes[i].vdisplay;
        if (pixels > best_pixels || (pixels == best_pixels && modes[i].vrefresh > best_refresh)) {
            best = static_cast<int>(i);
            best_pixels = pixels;
            best_refresh = modes[i].vrefresh;
        }
    }

    return best;
}

} // namespace helix
