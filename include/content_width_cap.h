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
 * @brief Widest a modal dialog may grow, as a ratio of screen height.
 *
 * Dialogs size themselves as a share of the screen width, which on an ultrawide
 * panel stretches a two-line message and its buttons across a thousand pixels.
 * Every ui_dialog is capped at kDialogMaxWidthNum / kDialogMaxWidthDen (1.8)
 * times the screen height. The widest dialog any standard screen shows (a fixed
 * 480 on 480x272) is 1.76 times its height, so the cap binds only on ultrawide
 * screens. Registered as the dialog_max_width XML const beside content_max_width.
 */
constexpr int32_t kDialogMaxWidthNum = 9;
constexpr int32_t kDialogMaxWidthDen = 5;

/// The dialog width cap for a screen of @p ver_res pixels tall.
constexpr int32_t dialog_max_width(int32_t ver_res) {
    return ver_res * kDialogMaxWidthNum / kDialogMaxWidthDen;
}

/**
 * @brief The height a dialog's scroll area should take so the dialog fits the screen.
 *
 * A dialog's content scrolls inside a box whose height is capped. A fixed cap is
 * wrong both ways: it scrolls content that would fit (a short screen with room to
 * spare) and lets a tall dialog run off the bottom. This grows the box by the
 * content it hides, as far as the room the screen has left, and shrinks it when
 * the dialog is taller than the screen allows.
 *
 * @param scroller_h  the scroll area's current height
 * @param hidden      content height the scroll area hides now
 * @param dialog_h    the dialog's current height
 * @param avail_h     the tallest the dialog may be (the screen less its margins)
 * @return the scroll area's new height, never negative
 */
constexpr int32_t fit_dialog_scroller(int32_t scroller_h, int32_t hidden, int32_t dialog_h,
                                      int32_t avail_h) {
    const int32_t room = avail_h - dialog_h;
    const int32_t change = room < 0 ? room : (hidden < room ? hidden : room);
    const int32_t h = scroller_h + change;
    return h < 0 ? 0 : h;
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
