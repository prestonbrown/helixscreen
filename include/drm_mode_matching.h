// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file drm_mode_matching.h
 * @brief Pure helpers for matching a requested resolution against a DRM
 *        connector's mode list.
 *
 * Kept independent of libdrm types so it is unit-testable without a real
 * DRM device. The DRM backend populates a std::vector<DrmModeInfo> from the
 * connector modes and passes it here.
 */

#include <cstdint>
#include <vector>

namespace helix {

/**
 * @brief Minimal POD representation of a DRM mode for matching purposes.
 *
 * We only need the dimensions (for matching against user's -s), the refresh
 * rate (for logging), and whether the mode is marked DRM_MODE_TYPE_PREFERRED
 * (for fallback selection).
 */
struct DrmModeInfo {
    uint32_t hdisplay;
    uint32_t vdisplay;
    uint32_t vrefresh;
    bool is_preferred;
};

/** @brief Sentinel returned when no match / no modes available. */
struct DrmModeMatch {
    static constexpr int NO_MATCH = -1;
};

/**
 * @brief Find a mode whose hdisplay/vdisplay exactly match the request.
 *
 * Refresh rate is ignored — multiple modes with the same pixel dimensions
 * are common and picking any of them is fine for our purposes.
 *
 * @return Index of matching mode, or DrmModeMatch::NO_MATCH if none match
 *         or the list is empty.
 */
int find_matching_mode(const std::vector<DrmModeInfo>& modes, uint32_t requested_w,
                       uint32_t requested_h);

/**
 * @brief Find the first mode marked as DRM preferred, falling back to 0.
 *
 * @return Index of preferred mode, 0 if none are marked preferred but the
 *         list is non-empty, or DrmModeMatch::NO_MATCH if the list is empty.
 */
int find_preferred_mode_index(const std::vector<DrmModeInfo>& modes);

/**
 * @brief Find the best lower-resolution mode when the preferred mode exceeds
 *        a pixel threshold on either axis.
 *
 * Used to auto-downscale phone-class panels (e.g. 1440x2560 QHD AMOLED)
 * that report absurdly high native resolutions via EDID. Selects the
 * highest-resolution mode where both axes are <= @p max_axis. Ties
 * (same pixel count) are broken by refresh rate (higher wins).
 *
 * @param max_axis  Maximum allowed pixels on either axis (e.g. 1920).
 * @return Index of the best downscale candidate, or DrmModeMatch::NO_MATCH
 *         if no downscaling is needed (preferred mode is within threshold)
 *         or no suitable lower mode exists.
 */
int find_best_downscale_mode(const std::vector<DrmModeInfo>& modes, uint32_t max_axis);

} // namespace helix
