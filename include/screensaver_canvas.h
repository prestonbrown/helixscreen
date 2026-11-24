// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_frame.h"
#include "screensaver_registry.h"

#include <cstddef>
#include <cstdint>
#include <lvgl.h>
#include <vector>

namespace helix::ui {
/**
 * @brief Canvas format for a saver that writes every pixel itself: the display's own depth
 *
 * Opaque, so the full-screen canvas draws as a straight copy instead of an alpha blend.
 * Production 32 bpp displays run ARGB8888 and copy an XRGB8888 image byte for byte, so every
 * canvas pixel's X byte must be 0xFF or that pixel draws transparent ("The alpha trap" in
 * docs/devel/GPU_ACCELERATION.md). PixelWriter writes every pixel with that byte set.
 */
inline constexpr lv_color_format_t kSaverBuildCanvasFormat =
    LV_COLOR_DEPTH == 16 ? LV_COLOR_FORMAT_RGB565 : LV_COLOR_FORMAT_XRGB8888;

/**
 * @brief Canvas format for a saver that also draws with lv_draw_* (text, say)
 *
 * On aarch64, LVGL's NEON blends into an XRGB8888 destination write 0 into each pixel's 4th
 * byte, and an XRGB8888 canvas is copied byte for byte into the ARGB8888 display, so those
 * pixels would draw transparent. An ARGB8888 canvas keeps real alpha, and it is filled opaque
 * before anything is drawn on it, so it still covers every pixel under the transparent overlay;
 * PixelWriter's XRGB8888 writes are opaque ARGB8888 pixels. RGB565 has no 4th byte for a blend
 * to zero, so a 16 bpp build takes the display's own format.
 */
inline constexpr lv_color_format_t kLvDrawCanvasFormat =
    LV_COLOR_DEPTH == 16 ? LV_COLOR_FORMAT_RGB565 : LV_COLOR_FORMAT_ARGB8888;

/// PixelWriter format of a canvas: RGB565 for an RGB565 canvas, XRGB8888 for a 4-byte one.
constexpr PixelFormat pixel_format_for(lv_color_format_t cf) {
    return cf == LV_COLOR_FORMAT_RGB565 ? PixelFormat::RGB565 : PixelFormat::XRGB8888;
}

/**
 * @brief A full-screen canvas a screensaver draws on
 *
 * Owns the draw buffer, allocated at the row stride lv_canvas_set_buffer() uses, so direct
 * pixel writes land where the canvas reads them. A scene drawn with lv_draw_* opens a layer
 * session and marks what it draws; a scene written pixel by pixel passes its dirty areas to
 * invalidate(). Either way only the changed areas are redrawn.
 */
class SaverCanvas {
  public:
    SaverCanvas() = default;
    SaverCanvas(const SaverCanvas&) = delete;
    SaverCanvas& operator=(const SaverCanvas&) = delete;

    /**
     * @brief Creates a w x h canvas in format `cf` as a child of `parent`, painted opaque black
     * @return false when the buffer cannot be allocated; nothing is created then
     */
    bool create(lv_obj_t* parent, int32_t w, int32_t h, lv_color_format_t cf);

    /// Hides the canvas, then frees its buffer. The canvas object is deleted with its parent.
    void release();

    lv_obj_t* obj() const {
        return canvas_;
    }
    uint8_t* data() const {
        return buf_;
    }
    uint32_t stride() const {
        return stride_;
    }
    size_t buffer_size() const {
        return buf_size_;
    }
    lv_color_format_t format() const {
        return cf_;
    }
    int32_t width() const {
        return w_;
    }
    int32_t height() const {
        return h_;
    }

    /// The buffer as a frame for direct pixel writes, for a canvas in a format PixelWriter writes.
    FrameTarget frame() const {
        return {buf_, stride_, static_cast<uint32_t>(w_), static_cast<uint32_t>(h_),
                pixel_format_for(cf_)};
    }

    /// Paints the whole canvas opaque black and invalidates all of it.
    void fill_black();

    /// Invalidates `areas`, in canvas pixels, after reduce_dirty() to kSaverMaxDirtyAreas: the
    /// whole canvas when they reduce to it.
    void invalidate(std::vector<DirtyRect>& areas);

    /// Opens a layer session for lv_draw_* calls on the canvas.
    void begin_layer(lv_layer_t* layer);

    /// Records a canvas area (inclusive) the open layer session draws into.
    void mark_dirty(int32_t x1, int32_t y1, int32_t x2, int32_t y2);

    /// Finishes the layer session and invalidates only the areas marked since begin_layer().
    void finish_layer(lv_layer_t* layer);

  private:
    lv_obj_t* canvas_ = nullptr;
    uint8_t* buf_ = nullptr;
    size_t buf_size_ = 0;
    uint32_t stride_ = 0;
    int32_t w_ = 0;
    int32_t h_ = 0;
    lv_color_format_t cf_ = LV_COLOR_FORMAT_UNKNOWN;
    std::vector<DirtyRect> layer_dirty_;
};

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
