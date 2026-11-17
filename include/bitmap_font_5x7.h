// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file bitmap_font_5x7.h
 * @brief A 5x7 ASCII bitmap font for drawing text into raw pixel buffers without LVGL
 *
 * The label printer draws labels with it, and the screen server draws its placeholder
 * frame with it.
 */

#pragma once

#include <cstdint>
#include <string_view>

namespace helix {

constexpr int kFont5x7W = 5;
constexpr int kFont5x7H = 7;

/// The 7 rows of @p c's glyph, top first; bit 7 of each row is the leftmost pixel.
/// Anything outside printable ASCII (32-126) draws as a space.
const uint8_t* font5x7_glyph(char c);

/// Pixel width of @p text at @p scale: glyphs sit one scaled pixel apart, and the last
/// has no trailing gap.
int font5x7_text_width(std::string_view text, int scale);

/// Call @p set_pixel(x, y) for every lit pixel of @p text drawn with its top-left corner
/// at (@p x, @p y), each font pixel a @p scale x @p scale block.
template <typename SetPixel>
void font5x7_draw_text(std::string_view text, int x, int y, int scale, SetPixel&& set_pixel) {
    const int pitch = (kFont5x7W + 1) * scale;
    for (const char c : text) {
        const uint8_t* glyph = font5x7_glyph(c);
        for (int row = 0; row < kFont5x7H; ++row) {
            for (int col = 0; col < kFont5x7W; ++col) {
                if ((glyph[row] & (0x80 >> col)) == 0) {
                    continue;
                }
                for (int sy = 0; sy < scale; ++sy) {
                    for (int sx = 0; sx < scale; ++sx) {
                        set_pixel(x + col * scale + sx, y + row * scale + sy);
                    }
                }
            }
        }
        x += pitch;
    }
}

} // namespace helix
