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
 * @brief Narrowest a main column may be beside a side band.
 *
 * Controls' three top cards and Filament's two columns still hold their content
 * at this width (screenshots at 1280x400, where the column is 684 beside a
 * 520 px band). Below it the host takes the whole width and shows no band.
 */
constexpr int32_t kMainColumnFloor = 680;

/**
 * @brief The side band's width, as a ratio of screen height.
 *
 * The side band (include/ui_side_band.h) hosts Motion or the AMS view beside a
 * host panel. Motion's speed buttons and the AMS lane labels need about one and
 * a third screen heights; a narrower band clips them, so the band is never
 * shown narrower than this.
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
 * The band is always side_band_pref_width() wide and the main column takes the
 * rest, up to the cap (1280x400: 684 + 520; 1920x480: 1152 + 692). The band
 * shows only while the column keeps kMainColumnFloor (or the whole cap when the
 * cap is smaller); otherwise the column takes the whole host and there is no
 * band. A wider screen never gets a narrower band than a narrower one.
 */
constexpr SideBandLayout side_band_layout(int32_t host_width, int32_t cap, int32_t ver_res) {
    int32_t main_width = host_width - side_band_pref_width(ver_res);
    if (main_width > cap) {
        main_width = cap;
    }
    const int32_t floor = cap < kMainColumnFloor ? cap : kMainColumnFloor;
    if (main_width >= floor) {
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
