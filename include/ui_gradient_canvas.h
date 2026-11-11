// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl/lvgl.h"

/**
 * @brief Register the ui_gradient_canvas widget with the LVGL XML system
 *
 * Creates an image widget that draws a procedural diagonal gradient from
 * start_color (top-right) to end_color (bottom-left). Drawing it, rather than
 * scaling a PNG, avoids scaling artifacts and banding. The buffer is rebuilt at
 * the widget's size (capped at kMaxGradientDim) whenever that size changes.
 *
 * XML attributes:
 *   - width, height: Widget size; the gradient buffer follows it
 *   - dither: "true" (the default) for ordered dithering on 16-bit displays
 *   - start_color: Hex color for the top-right corner (default: the theme's)
 *   - end_color: Hex color for the bottom-left corner (default: the theme's)
 *
 * Color format: Standard LVGL hex colors - "RRGGBB" or "#RRGGBB" (e.g., "FF00FF" for magenta)
 *
 * Example:
 *   <ui_gradient_canvas width="100%" height="100%" dither="true"
 *                       start_color="00FFFF" end_color="FF00FF"/>
 *
 * Must be called before any XML files using <ui_gradient_canvas> are loaded.
 */
void ui_gradient_canvas_register(void);

/**
 * @brief Redraw the gradient on an existing canvas
 *
 * Call this if the canvas is resized or needs to be refreshed.
 *
 * @param canvas Pointer to the canvas object
 */
void ui_gradient_canvas_redraw(lv_obj_t* canvas);

/**
 * @brief Redraw all gradient canvas widgets for a theme change
 *
 * Walks the widget tree from root, finds all ui_gradient_canvas widgets
 * (identified by user_data containing GradientData), and re-renders them
 * with current theme colors.
 *
 * @param root Root object to walk (typically lv_screen_active())
 */
void ui_gradient_canvas_theme_update(lv_obj_t* root);

/**
 * @brief Enable or disable dithering for a gradient canvas
 *
 * @param canvas Pointer to the canvas object
 * @param enable true to enable ordered dithering
 */
void ui_gradient_canvas_set_dither(lv_obj_t* canvas, bool enable);

/**
 * @brief Create a gradient draw buffer at exact dimensions
 *
 * Renders a diagonal gradient (bright top-right, dark bottom-left) into an
 * ARGB8888 draw buffer at the specified dimensions. The returned buffer can
 * be shared across multiple lv_image widgets via lv_image_set_src().
 *
 * Caller owns the returned buffer and must call lv_draw_buf_destroy() when done.
 *
 * @param width Buffer width in pixels
 * @param height Buffer height in pixels
 * @param dark_mode true for dark theme colors, false for light
 * @param radius Corner radius in pixels (0 = no rounding)
 * @return Owned lv_draw_buf_t*, or nullptr on allocation failure
 */
lv_draw_buf_t* ui_gradient_canvas_create_buf(int32_t width, int32_t height, bool dark_mode,
                                             int32_t radius);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
namespace helix::ui {
/**
 * @brief Create the same gradient flattened onto a solid background color
 *
 * The rounded-corner pixels are composited against @p behind and the result is
 * stored in the display's native color format with no alpha, so LVGL draws it
 * as a plain copy instead of a per-pixel blend. Identical on screen to
 * ui_gradient_canvas_create_buf() only where the area behind the image is that
 * solid color.
 *
 * @param behind The solid color the image's corners sit on
 * @param under  The color of the rounded background drawn under the image, whose
 *               anti-aliased edge shows through the corner fringe
 * @return Owned lv_draw_buf_t*, or nullptr on allocation failure
 */
lv_draw_buf_t* gradient_canvas_create_opaque_buf(int32_t width, int32_t height, bool dark_mode,
                                                 int32_t radius, lv_color_t behind,
                                                 lv_color_t under);

/**
 * @brief Render the diagonal gradient into an existing buffer
 *
 * @p start is the top-right color, @p end the bottom-left. Writes every pixel
 * opaque; @p buf may be ARGB8888, XRGB8888 or RGB565.
 */
void gradient_canvas_render(lv_draw_buf_t* buf, lv_color_t start, lv_color_t end, bool dither);
} // namespace helix::ui
#endif
