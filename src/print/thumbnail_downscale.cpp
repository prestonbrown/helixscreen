// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "thumbnail_downscale.h"

#include "lvgl.h"

#include <algorithm>
#include <new>

namespace helix {

ThumbnailDims fit_thumbnail(int w, int h, int max_w, int max_h) {
    if (w <= 0 || h <= 0 || max_w <= 0 || max_h <= 0) {
        return {};
    }
    if (w <= max_w && h <= max_h) {
        return {w, h};
    }
    // Width is the binding side when w/h >= max_w/max_h.
    if (static_cast<int64_t>(w) * max_h >= static_cast<int64_t>(h) * max_w) {
        return {max_w, std::max(1, static_cast<int>(static_cast<int64_t>(h) * max_w / w))};
    }
    return {std::max(1, static_cast<int>(static_cast<int64_t>(w) * max_h / h)), max_h};
}

namespace {

/// First source index of output cell @p d when @p src cells map onto @p dst.
int box_start(int d, int src, int dst) {
    return static_cast<int>(static_cast<int64_t>(d) * src / dst);
}

} // namespace

RowDownscaler::RowDownscaler(int src_w, int src_h, ThumbnailDims dst, uint8_t* out)
    : src_w_(src_w), src_h_(src_h), dst_(dst), out_(out),
      x0_(new(std::nothrow) int[static_cast<size_t>(dst.w)]),
      x1_(new(std::nothrow) int[static_cast<size_t>(dst.w)]),
      sums_(new(std::nothrow) uint32_t[static_cast<size_t>(dst.w) * 4]()) {
    if (!ok()) {
        return;
    }
    for (int dx = 0; dx < dst.w; ++dx) {
        x0_[static_cast<size_t>(dx)] = box_start(dx, src_w, dst.w);
        x1_[static_cast<size_t>(dx)] =
            std::max(x0_[static_cast<size_t>(dx)] + 1, box_start(dx + 1, src_w, dst.w));
    }
}

RowDownscaler::RowDownscaler(int src_w, int src_h, ThumbnailDims dst, uint16_t* onto,
                             int onto_stride)
    : RowDownscaler(src_w, src_h, dst, reinterpret_cast<uint8_t*>(onto)) {
    onto_stride_ = onto_stride;
}

void RowDownscaler::add_row(const uint8_t* rgba) {
    if (!ok() || src_y_ >= src_h_ || dst_y_ >= dst_.h) {
        return;
    }
    for (int dx = 0; dx < dst_.w; ++dx) {
        uint32_t* s = &sums_[static_cast<size_t>(dx) * 4];
        const uint8_t* p = rgba + static_cast<size_t>(x0_[static_cast<size_t>(dx)]) * 4;
        for (int x = x0_[static_cast<size_t>(dx)]; x < x1_[static_cast<size_t>(dx)]; ++x, p += 4) {
            s[0] += p[0] * p[3];
            s[1] += p[1] * p[3];
            s[2] += p[2] * p[3];
            s[3] += p[3];
        }
    }
    ++src_y_;

    const int y0 = box_start(dst_y_, src_h_, dst_.h);
    const int y1 = std::max(y0 + 1, box_start(dst_y_ + 1, src_h_, dst_.h));
    if (src_y_ < y1) {
        return;
    }
    auto* colour = reinterpret_cast<uint16_t*>(out_);
    uint8_t* alpha = out_ + static_cast<size_t>(dst_.w) * dst_.h * 2;
    for (int dx = 0; dx < dst_.w; ++dx) {
        uint32_t* s = &sums_[static_cast<size_t>(dx) * 4];
        const uint32_t n = static_cast<uint32_t>(
            (y1 - y0) * (x1_[static_cast<size_t>(dx)] - x0_[static_cast<size_t>(dx)]));
        uint16_t c = 0;
        uint8_t a = 0;
        if (s[3] != 0) {
            const uint32_t r8 = s[0] / s[3], g8 = s[1] / s[3], b8 = s[2] / s[3];
            c = static_cast<uint16_t>(((r8 >> 3) << 11) | ((g8 >> 2) << 5) | (b8 >> 3));
            a = static_cast<uint8_t>(s[3] / n);
        }
        if (onto_stride_) {
            uint16_t& px = colour[static_cast<size_t>(dst_y_) * onto_stride_ + dx];
            px = lv_color_16_16_mix(c, px, a);
        } else {
            const size_t i = static_cast<size_t>(dst_y_) * dst_.w + dx;
            colour[i] = c;
            alpha[i] = a;
        }
        s[0] = s[1] = s[2] = s[3] = 0;
    }
    ++dst_y_;
}

void downscale_rgba_to_rgb565a8(const uint8_t* rgba, int src_w, int src_h, ThumbnailDims dst,
                                uint8_t* out) {
    RowDownscaler scaler(src_w, src_h, dst, out);
    for (int y = 0; y < src_h; ++y) {
        scaler.add_row(rgba + static_cast<size_t>(y) * src_w * 4);
    }
}

} // namespace helix
