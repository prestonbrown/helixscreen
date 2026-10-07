// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Scroll blit: on a display that keeps its last rendered frame, a scroll moves
// the pixels already there and renders only the strip that scrolled into view,
// instead of rendering the whole scroller again every frame of a drag.

#include "lvgl/lvgl.h"

#include <cstddef>
#include <cstdint>

namespace helix {

/// A display's last rendered frame, in memory the renderer writes chunks into
/// and never clears: full screen, row-major, the display's color format.
struct RetainedFrame {
    uint8_t* buf = nullptr;
    size_t stride = 0;          ///< bytes per row
    uint8_t* scratch = nullptr; ///< bounce rows for the copy; null copies row to row
    size_t scratch_bytes = 0;
    /// Takes the frame's rows [y1, y2] for writing until the next render
    /// finishes, and has them shown with it. False when the frame cannot be
    /// taken; the scroll then renders in full.
    bool (*claim_rows)(int32_t y1, int32_t y2) = nullptr;
};

/// Scrolls on `disp` blit wherever the scroller's region holds only its own
/// content over a flat background. Everything else renders as before.
void scroll_blit_install(lv_display_t* disp, const RetainedFrame& frame);
void scroll_blit_uninstall();

/// Follow `obj`'s scrolls from now on. Scrollers being dragged are followed
/// automatically; a drag's first frame renders in full, since the position it
/// started from was not recorded.
void scroll_blit_track(lv_obj_t* obj);

/// The screen region whose pixels move with `obj`'s content, or false when a
/// blit there cannot match a full render (a gradient under it, a layer, a
/// custom draw handler) or would not pay (static widgets over half of it).
/// Static widgets painting in it are redrawn after the copy.
bool scroll_blit_region(lv_obj_t* obj, lv_area_t* out);

/// Moves the pixels of `area` by `dy` rows inside a frame. Rows leaving the
/// area are dropped; the |dy| rows it exposes keep their old pixels.
void shift_rows(uint8_t* buf, size_t stride, uint32_t px_bytes, const lv_area_t& area, int32_t dy,
                uint8_t* scratch, size_t scratch_bytes);

} // namespace helix
