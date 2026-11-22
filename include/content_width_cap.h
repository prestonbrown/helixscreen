// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace helix {

/**
 * @brief Widest a panel or overlay's content may grow, as a ratio of screen height.
 *
 * Panels and overlays fill the space beside the nav bar. On an ultrawide panel
 * (1480x320, 1920x440) that is three or four screen-heights across, and a
 * settings list or a jog pad stretched that far is hard to read and wastes
 * reach. Content is capped at kContentMaxWidthNum / kContentMaxWidthDen (2.4)
 * times the screen height and kept against the nav bar; the rest of the width
 * stays empty.
 *
 * The ratio is held as a fraction so the cap is exact integer arithmetic: 2.4
 * as a double times 320 truncates to 767.
 *
 * 800x480, 1024x600 and every portrait screen sit well under the ratio, so the
 * cap never binds there. Registered as the content_max_width XML const by
 * theme_manager_register_responsive_spacing() and refreshed on resize.
 */
constexpr int32_t kContentMaxWidthNum = 12;
constexpr int32_t kContentMaxWidthDen = 5;

/// The content width cap for a screen of @p ver_res pixels tall.
constexpr int32_t content_max_width(int32_t ver_res) {
    return ver_res * kContentMaxWidthNum / kContentMaxWidthDen;
}

/**
 * @brief Narrowest the ultrawide side band may be, as a ratio of screen height.
 *
 * The side band (include/ui_side_band.h) hosts Motion or the AMS view in the
 * width the cap leaves beside a host panel. Those views are laid out for a box
 * about one and a half screen heights wide; at one screen height Motion's jog
 * pad still fits beside its Z column, and below that its step buttons and
 * readout run off the screen. An ultrawide screen only just past the layout's
 * 2.5:1 threshold (1280x480, 1920x720) leaves a sliver of under 120 px, so the
 * band shows only when the leftover reaches this width.
 */
constexpr int32_t kSideBandMinWidthNum = 1;
constexpr int32_t kSideBandMinWidthDen = 1;

/// The narrowest side band for a screen of @p ver_res pixels tall.
constexpr int32_t side_band_min_width(int32_t ver_res) {
    return ver_res * kSideBandMinWidthNum / kSideBandMinWidthDen;
}

/// Whether a host @p host_width wide, its main column capped at @p cap, leaves
/// a side band at least side_band_min_width(@p ver_res) wide.
constexpr bool side_band_has_room(int32_t host_width, int32_t cap, int32_t ver_res) {
    return host_width - cap >= side_band_min_width(ver_res);
}

/**
 * @brief How far a view of @p width overshoots the cap.
 *
 * An overlay keeps its full opaque width, so it occludes the backdrop as
 * before, and moves this many pixels into its right padding. Zero when the
 * view fits, which is every view on a non-ultrawide screen.
 */
constexpr int32_t content_cap_overflow(int32_t width, int32_t cap) {
    return (cap > 0 && width > cap) ? width - cap : 0;
}

} // namespace helix
