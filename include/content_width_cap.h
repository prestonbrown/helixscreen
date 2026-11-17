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
