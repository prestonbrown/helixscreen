// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"

#include <cstdint>

namespace helix::ui {

/// Returns an lv_image widget containing a blurred snapshot of the current
/// screen, or nullptr if blur is unavailable or permanently disabled.
/// On failure, permanently disables blur for the rest of the app lifecycle
/// (circuit breaker pattern).
///
/// @param parent  Parent object for the image widget
/// @param dim_opacity  Opacity of the dark tint overlay (0-255)
/// @return Image widget with blurred backdrop, or nullptr (caller should fall back)
lv_obj_t* create_blurred_backdrop(lv_obj_t* parent, lv_opa_t dim_opacity);

/// Returns a fully opaque lv_image widget containing a darkened snapshot of the
/// current screen.  The snapshot is taken once and darkened in-place — no
/// per-frame opacity blending cost.
///
/// Where snapshots are off (ESP32, whose 8MB PSRAM cannot spare a full-frame
/// copy for a dimmed navbar) or once one has failed to allocate, returns
/// create_dim_layer() instead. Returns nullptr only without a parent or screen.
///
/// @param parent  Parent object for the image widget
/// @param dim_opacity  Dimming amount (same scale as bg_opa: 0=no dim, 255=black)
lv_obj_t* create_darkened_backdrop(lv_obj_t* parent, lv_opa_t dim_opacity);

/// Re-take the snapshot of a create_darkened_backdrop() image into the buffer it
/// already owns, so a refresh never holds a second full frame. Returns false,
/// leaving the old pixels, when @p backdrop is not a snapshot image or the
/// screen no longer fits its buffer.
bool retake_darkened_backdrop(lv_obj_t* backdrop, lv_opa_t dim_opacity);

/// Full-size, clickable, translucent black rectangle. Costs no pixel buffer, but
/// LVGL blends whatever redraws beneath it every frame.
lv_obj_t* create_dim_layer(lv_obj_t* parent, lv_opa_t dim_opacity);

/// Free cached GPU resources (shaders, FBOs, textures).
/// Also resets the circuit breaker, allowing blur to be retried.
/// Call on shutdown or display resize.
void backdrop_blur_cleanup();

// ---- Internal helpers exposed for testing ----

namespace detail {

/// Box blur a single-channel or multi-channel ARGB8888 buffer in-place.
/// @param data     Pixel buffer (ARGB8888 format, 4 bytes per pixel)
/// @param width    Image width in pixels
/// @param height   Image height in pixels
/// @param iterations  Number of box blur passes (3 ≈ Gaussian σ≈2.5)
void box_blur_argb8888(uint8_t* data, int width, int height, int iterations = 3);

/// Downscale an ARGB8888 buffer by 2x using 2x2 averaging.
/// Caller must allocate dst with (width/2) * (height/2) * 4 bytes.
/// @param src       Source pixel buffer
/// @param dst       Destination pixel buffer (half dimensions)
/// @param src_width  Source width (must be even)
/// @param src_height Source height (must be even)
void downscale_2x_argb8888(const uint8_t* src, uint8_t* dst, int src_width, int src_height,
                           int src_stride);

/// Darken ARGB8888 pixels in-place.  Each RGB channel is multiplied by
/// (255 - dim_opacity) / 255.  Alpha channel is set to 255 (fully opaque).
/// This replicates the visual effect of a black overlay with the given opacity
/// but costs zero per-frame blending.
void darken_argb8888_inplace(uint8_t* data, int width, int height, int stride,
                             lv_opa_t dim_opacity);

/// Darken RGB565 pixels in-place, the same way as darken_argb8888_inplace().
void darken_rgb565_inplace(uint8_t* data, int width, int height, int stride, lv_opa_t dim_opacity);

/// Reset the circuit breaker (for testing only).
void reset_circuit_breaker();

/// Check if blur is permanently disabled.
bool is_blur_disabled();

/// Whether create_darkened_backdrop() takes snapshots (set it for testing only).
bool snapshot_backdrops_enabled();
void set_snapshot_backdrops_enabled(bool enabled);

} // namespace detail
} // namespace helix::ui
