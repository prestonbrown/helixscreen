// SPDX-License-Identifier: GPL-3.0-or-later
/// @file toolhead_art.h
/// @brief Baked toolhead drawings, composed once per style, size and filament colour.

#pragma once

#include "lvgl/lvgl.h"
#include "toolhead_style.h"

#include <cstdint>
#include <optional>

namespace helix::ui {

/// One baked head, from src/generated/toolhead_art_gen.cpp.
struct ToolheadArt {
    ToolheadStyle style;
    const char* id;
    float aspect; ///< width over height
    float tip_x;  ///< the nozzle tip's column, as a share of the width
    float tip_y;  ///< the nozzle tip's row, as a share of the height
};
extern const ToolheadArt kToolheadArt[];
extern const int kToolheadArtCount;

/// The art for @p style, or nullptr when the style keeps its vector renderer.
const ToolheadArt* toolhead_art_for(ToolheadStyle style);

struct ToolheadArtRect {
    int32_t x, y, w, h;
};
/// Where a head draws: 8 scale units tall, centred on @p nozzle_y, nozzle tip on @p cx.
ToolheadArtRect toolhead_art_rect(const ToolheadArt& art, int32_t cx, int32_t nozzle_y,
                                  int32_t scale);

/// Paints @p tint through @p mask, then @p shade, over @p body. All are ARGB8888, @p w by @p h.
void toolhead_art_compose(uint8_t* body, const uint8_t* mask, const uint8_t* shade, int32_t w,
                          int32_t h, lv_color_t tint);

/// Area-averages ARGB8888 @p src to @p dst, weighting colour by alpha so edges stay clean.
void toolhead_art_resample(const uint8_t* src, int32_t src_w, int32_t src_h, uint8_t* dst,
                           int32_t dst_w, int32_t dst_h);

/// Draws @p style's art as one image. False when the style has none or its files cannot load.
bool draw_toolhead_art(lv_layer_t* layer, ToolheadStyle style, int32_t cx, int32_t nozzle_y,
                       std::optional<lv_color_t> filament, int32_t scale, lv_opa_t opa);

/// How many images have been composed since start; a redraw of a known head adds none.
unsigned toolhead_art_build_count();

} // namespace helix::ui
