// SPDX-License-Identifier: GPL-3.0-or-later
/// @file baked_art.h
/// @brief Pre-rendered art layers: decoded once, scaled to the size drawn, cached while in use.

#pragma once

#include "lvgl/lvgl.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace helix::ui {

/// One art layer's ARGB8888 pixels with no row padding. Empty (w == 0) when it could not load.
struct BakedPixels {
    std::vector<uint8_t> data;
    int32_t w = 0;
    int32_t h = 0;
};

/// Decodes the art file at @p path (relative to the asset root); logs @p who on failure.
BakedPixels baked_art_decode(const std::string& path, const char* who);

/// Area-averages ARGB8888 @p src to @p dst, weighting colour by alpha so edges stay clean.
void baked_art_resample(const uint8_t* src, int32_t src_w, int32_t src_h, uint8_t* dst,
                        int32_t dst_w, int32_t dst_h);

/// A new draw buffer holding @p src resampled to @p w x @p h, or nullptr.
lv_draw_buf_t* baked_art_buffer(const BakedPixels& src, int32_t w, int32_t h);

/// The image cached under @p key, made by @p build on a miss. Images not drawn for a while are
/// dropped once the cache is full. nullptr when @p build fails.
lv_draw_buf_t* baked_art_cached(const std::string& key,
                                const std::function<lv_draw_buf_t*()>& build);

/// Draws @p buf with its top-left corner at (@p x, @p y).
void baked_art_draw(lv_layer_t* layer, lv_draw_buf_t* buf, int32_t x, int32_t y,
                    lv_opa_t opa = LV_OPA_COVER);

} // namespace helix::ui
