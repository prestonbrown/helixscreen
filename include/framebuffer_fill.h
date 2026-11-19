// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace helix {

/// Bits per pixel a framebuffer clear fills by, read from the stride (32 when
/// xres is 0). The mapped clear and the signal-safe clear must agree on it.
inline uint32_t framebuffer_fill_bpp(uint32_t xres, uint32_t line_length) {
    return xres > 0 ? (line_length * 8) / xres : 32;
}

/// Fill @p bytes of a mapped framebuffer with @p color (ARGB, 0xAARRGGBB).
/// 32bpp writes the pixel as given, 16bpp converts to RGB565, and any other
/// depth is zeroed.
inline void fill_framebuffer(void* fbp, size_t bytes, uint32_t bpp, uint32_t color) {
    if (bpp == 32) {
        auto* pixels = static_cast<uint32_t*>(fbp);
        const size_t pixel_count = bytes / 4;
        for (size_t i = 0; i < pixel_count; i++) {
            pixels[i] = color;
        }
    } else if (bpp == 16) {
        const uint16_t r = ((color >> 16) & 0xFF) >> 3;
        const uint16_t g = ((color >> 8) & 0xFF) >> 2;
        const uint16_t b = (color & 0xFF) >> 3;
        const uint16_t rgb565 = (r << 11) | (g << 5) | b;

        auto* pixels = static_cast<uint16_t*>(fbp);
        const size_t pixel_count = bytes / 2;
        for (size_t i = 0; i < pixel_count; i++) {
            pixels[i] = rgb565;
        }
    } else {
        memset(fbp, 0, bytes);
    }
}

} // namespace helix
