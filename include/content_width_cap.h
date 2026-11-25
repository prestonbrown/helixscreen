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
 * band shows only when the host can spare this width (side_band_layout()).
 */
constexpr int32_t kSideBandMinWidthNum = 1;
constexpr int32_t kSideBandMinWidthDen = 1;

/// The narrowest side band for a screen of @p ver_res pixels tall.
constexpr int32_t side_band_min_width(int32_t ver_res) {
    return ver_res * kSideBandMinWidthNum / kSideBandMinWidthDen;
}

/**
 * @brief Narrowest a main column may be beside a side band.
 *
 * Controls' three top cards and Filament's two columns still hold their content
 * at this width (screenshots at 1280x400, where the column is 684 beside a
 * 520 px band). Below it the host takes the whole width and shows no band.
 */
constexpr int32_t kMainColumnFloor = 680;

/**
 * @brief The band's preferred width, as a ratio of screen height.
 *
 * Motion's speed buttons and the AMS lane labels need about one and a third
 * screen heights; the bare minimum (kSideBandMinWidthNum / Den) only keeps the jog pad.
 */
constexpr int32_t kSideBandPrefWidthNum = 13;
constexpr int32_t kSideBandPrefWidthDen = 10;

/// The band's preferred width for a screen of @p ver_res pixels tall.
constexpr int32_t side_band_pref_width(int32_t ver_res) {
    return ver_res * kSideBandPrefWidthNum / kSideBandPrefWidthDen;
}

/**
 * @brief Widest a page with no side band stays capped, in pixels.
 *
 * The 2.4 x height cap keeps a settings list readable on a screen far wider
 * than tall, but below this width it only leaves an empty strip (1280x400
 * and 1920x480 are the shipped cases). The cap binds only on a host wider
 * than this, where it limits the column to max(cap, kUncappedWidth).
 */
constexpr int32_t kUncappedWidth = 1920;

/// The width of a page without a side band: the whole host, unless the host is
/// wider than kUncappedWidth and the cap @p cap holds the column back.
constexpr int32_t panel_content_width(int32_t host_width, int32_t cap) {
    const int32_t limit = cap > kUncappedWidth ? cap : kUncappedWidth;
    return host_width <= limit ? host_width : limit;
}

/// How a page with a side band divides its host.
struct SideBandLayout {
    int32_t main_width; ///< the main column's width
    bool room;          ///< the band fits beside it
};

/**
 * @brief The one rule for a page with a side band, for a host @p host_width
 * wide on a screen @p ver_res tall whose content cap is @p cap.
 *
 * - The cap leaves side_band_min_width(): the column stops at the cap, the band
 *   takes the rest.
 * - Otherwise the column gives up width to a band of up to
 *   side_band_pref_width(), as long as it keeps kMainColumnFloor and the band
 *   keeps side_band_min_width() (1280x400: 684 + 520).
 * - Otherwise the column takes the whole host and there is no band.
 */
constexpr SideBandLayout side_band_layout(int32_t host_width, int32_t cap, int32_t ver_res) {
    const int32_t band_min = side_band_min_width(ver_res);
    if (host_width - cap >= band_min) {
        return {cap, true};
    }
    int32_t main_width = host_width - side_band_pref_width(ver_res);
    if (main_width < kMainColumnFloor) {
        main_width = kMainColumnFloor;
    }
    if (host_width - main_width >= band_min) {
        return {main_width, true};
    }
    return {host_width, false};
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
