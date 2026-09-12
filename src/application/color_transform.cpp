// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "color_transform.h"

#include <algorithm>
#include <cmath>

namespace helix {

namespace {
inline uint8_t clamp_u8(int v) {
    return static_cast<uint8_t>(std::clamp(v, 0, 255));
}
} // namespace

ColorTransform::FlushRegion
ColorTransform::select_flush_region(const lv_draw_buf_t* active_buf, const lv_area_t& area,
                                    lv_color_format_t cf, lv_display_render_mode_t render_mode) {
    FlushRegion r;
    // The active buffer's real pitch, not one computed from the area width:
    // the DRM backend renders into dumb buffers whose pitch is kernel-aligned
    // and can exceed the area width (direct/full render mode). Fall back to
    // the computed stride only when the active buffer can't be queried.
    r.stride_bytes = (active_buf && active_buf->header.stride > 0)
                         ? active_buf->header.stride
                         : lv_draw_buf_width_to_stride(lv_area_get_width(&area), cf);
    // Partial mode reshapes the draw buffer to the dirty area and flushes
    // from the area's origin; direct and full keep the buffer at the
    // display's (0,0), so the rect sits at its absolute coordinates.
    if (render_mode == LV_DISPLAY_RENDER_MODE_PARTIAL) {
        r.x = 0;
        r.y = 0;
    } else {
        r.x = area.x1;
        r.y = area.y1;
    }
    return r;
}

void ColorTransform::reset() {
    for (int i = 0; i < 256; ++i) {
        r_lut_[i] = g_lut_[i] = b_lut_[i] = static_cast<uint8_t>(i);
    }
    identity_ = true;
}

void ColorTransform::set_panel_gain(float r_gain, float g_gain, float b_gain) {
    panel_r_gain_ = std::clamp(r_gain, 0.1f, 1.0f);
    panel_g_gain_ = std::clamp(g_gain, 0.1f, 1.0f);
    panel_b_gain_ = std::clamp(b_gain, 0.1f, 1.0f);
}

void ColorTransform::set(float gamma, int warmth, int tint) {
    gamma = std::clamp(gamma, 0.5f, 2.0f);
    warmth = std::clamp(warmth, -50, 50);
    tint = std::clamp(tint, -50, 50);

    const bool gain_is_identity =
        panel_r_gain_ >= 0.999f && panel_g_gain_ >= 0.999f && panel_b_gain_ >= 0.999f;

    if (std::fabs(gamma - 1.0f) < 0.01f && warmth == 0 && tint == 0 && gain_is_identity) {
        reset();
        return;
    }

    // warmth ±50 = ±25% R/B opposing shift (white-balance temperature)
    // tint   ±50 = ±25% G shift opposite R/B (white-balance tint, fixes
    //              magenta/green casts that warmth can't touch)
    // panel_*_gain is a fixed factory baseline applied multiplicatively on top —
    // lets a preset bake panel-specific white-point correction beyond what the
    // ±25% slider range can express.
    const float r_scale = (1.0f + warmth * 0.005f - tint * 0.0025f) * panel_r_gain_;
    const float g_scale = (1.0f + tint * 0.005f) * panel_g_gain_;
    const float b_scale = (1.0f - warmth * 0.005f - tint * 0.0025f) * panel_b_gain_;
    const float inv_gamma = 1.0f / gamma;

    for (int i = 0; i < 256; ++i) {
        const float n = static_cast<float>(i) / 255.0f;
        const float curved = std::pow(n, inv_gamma) * 255.0f;
        r_lut_[i] = clamp_u8(static_cast<int>(std::lround(curved * r_scale)));
        g_lut_[i] = clamp_u8(static_cast<int>(std::lround(curved * g_scale)));
        b_lut_[i] = clamp_u8(static_cast<int>(std::lround(curved * b_scale)));
    }
    identity_ = false;
}

void ColorTransform::apply_area(uint8_t* buf, int buf_stride_bytes, int x, int y, int w, int h,
                                lv_color_format_t cf) const {
    if (identity_ || !buf || w <= 0 || h <= 0) {
        return;
    }

    if (cf == LV_COLOR_FORMAT_RGB565) {
        // RGB565: 5 bits R, 6 bits G, 5 bits B. Expand to 8-bit, look up,
        // then re-quantize. Conventional expansion uses the high bits to fill
        // the low bits ("(c<<3)|(c>>2)" for 5-bit, "(c<<2)|(c>>4)" for 6-bit).
        for (int row = 0; row < h; ++row) {
            uint16_t* line = reinterpret_cast<uint16_t*>(buf + (y + row) * buf_stride_bytes) + x;
            for (int col = 0; col < w; ++col) {
                const uint16_t px = line[col];
                const int r5 = (px >> 11) & 0x1F;
                const int g6 = (px >> 5) & 0x3F;
                const int b5 = px & 0x1F;
                const int r8 = (r5 << 3) | (r5 >> 2);
                const int g8 = (g6 << 2) | (g6 >> 4);
                const int b8 = (b5 << 3) | (b5 >> 2);
                const int r_out = r_lut_[r8];
                const int g_out = g_lut_[g8];
                const int b_out = b_lut_[b8];
                line[col] = static_cast<uint16_t>(((r_out & 0xF8) << 8) | ((g_out & 0xFC) << 3) |
                                                  (b_out >> 3));
            }
        }
        return;
    }

    // ARGB8888 / XRGB8888 — 4 bytes per pixel, channel order is B,G,R,A in memory
    // (LVGL native order on little-endian).
    if (cf == LV_COLOR_FORMAT_ARGB8888 || cf == LV_COLOR_FORMAT_XRGB8888) {
        for (int row = 0; row < h; ++row) {
            uint8_t* line = buf + (y + row) * buf_stride_bytes + x * 4;
            for (int col = 0; col < w; ++col) {
                line[0] = b_lut_[line[0]];
                line[1] = g_lut_[line[1]];
                line[2] = r_lut_[line[2]];
                line += 4;
            }
        }
        return;
    }

    // RGB888 — 3 bytes per pixel, B,G,R order
    if (cf == LV_COLOR_FORMAT_RGB888) {
        for (int row = 0; row < h; ++row) {
            uint8_t* line = buf + (y + row) * buf_stride_bytes + x * 3;
            for (int col = 0; col < w; ++col) {
                line[0] = b_lut_[line[0]];
                line[1] = g_lut_[line[1]];
                line[2] = r_lut_[line[2]];
                line += 3;
            }
        }
        return;
    }
    // Other formats: silently skip — better to ship pixels untransformed
    // than to corrupt them.
}

} // namespace helix
