// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace helix {

struct ThumbnailDims {
    int w = 0;
    int h = 0;
};

/// The size an image of @p w x @p h takes inside a @p max_w x @p max_h box,
/// aspect kept. Never upscales; each side is at least 1. {0,0} for a degenerate
/// input.
ThumbnailDims fit_thumbnail(int w, int h, int max_w, int max_h);

/// Why a thumbnail decode produced nothing.
enum class ThumbnailDecodeFailure {
    None,
    OutOfMemory, ///< Worth retrying once memory frees up
    BadImage,    ///< Corrupt, truncated or unsupported: retrying cannot help
    Unsupported, ///< A kind of image the decoder does not take: retrying cannot help
};

/// Bytes an RGB565A8 image of @p dims needs: a 16-bit colour plane followed by
/// an 8-bit alpha plane.
inline size_t rgb565a8_size(ThumbnailDims dims) {
    return static_cast<size_t>(dims.w) * static_cast<size_t>(dims.h) * 3;
}

/// Bytes an opaque RGB565 image of @p dims needs.
inline size_t rgb565_size(ThumbnailDims dims) {
    return static_cast<size_t>(dims.w) * static_cast<size_t>(dims.h) * 2;
}

/// Box-filters RGBA8888 rows (bytes R,G,B,A per pixel), fed top to bottom, down
/// to @p dst, writing LVGL's RGB565A8 layout into @p out: dst.w * dst.h
/// native-endian RGB565 pixels, then dst.w * dst.h alpha bytes. Colour is
/// averaged weighted by alpha, so transparent pixels do not darken edges. Holds
/// one output row of sums, so a source never has to exist whole. @p out must
/// hold rgb565a8_size(dst); @p dst must not exceed the source.
class RowDownscaler {
  public:
    RowDownscaler(int src_w, int src_h, ThumbnailDims dst, uint8_t* out);
    /// The same filter blended onto the RGB565 pixels already at @p onto, rows
    /// @p onto_stride pixels apart, exactly as LVGL draws an RGB565A8 image over
    /// them, so the result is opaque and draws as a plain copy.
    RowDownscaler(int src_w, int src_h, ThumbnailDims dst, uint16_t* onto, int onto_stride);
    /// False when the per-column state could not be allocated.
    bool ok() const {
        return x0_ && x1_ && sums_;
    }
    /// The next source row, src_w RGBA pixels.
    void add_row(const uint8_t* rgba);
    /// True once every source row has been added.
    bool complete() const {
        return src_y_ == src_h_;
    }

  private:
    int src_w_, src_h_;
    ThumbnailDims dst_;
    uint8_t* out_;
    int onto_stride_ = 0; ///< 0 writes RGB565A8 planes; otherwise blends onto RGB565 rows
    int src_y_ = 0;
    int dst_y_ = 0;
    std::unique_ptr<int[]> x0_;        ///< first source column of each output column
    std::unique_ptr<int[]> x1_;        ///< one past its last
    std::unique_ptr<uint32_t[]> sums_; ///< r, g, b, a per output column of the open row
};

/// Box-filters a whole RGBA8888 image (rows packed) the same way.
void downscale_rgba_to_rgb565a8(const uint8_t* rgba, int src_w, int src_h, ThumbnailDims dst,
                                uint8_t* out);

} // namespace helix
