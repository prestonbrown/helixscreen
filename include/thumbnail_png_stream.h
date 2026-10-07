// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A PNG decoded a row at a time. A streaming inflater hands over the scanline
// bytes in pieces through a 32KB window, each finished row is unfiltered
// against the one above it and passed on as RGBA, and the downscale consumes
// the rows as they come: the decode never holds the image, which is what lets
// a thumbnail decode on a heap with no large free block. Every buffer is a
// checked nothrow allocation, so running out of memory fails the decode rather
// than aborting a build without exceptions.

#include "thumbnail_downscale.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>

namespace helix {

/// Largest thumbnail side decoded; anything larger keeps the placeholder.
inline constexpr int THUMBNAIL_MAX_SIDE = 640;

struct PngHeader {
    int width = 0;
    int height = 0;
    int bit_depth = 0;
    int color_type = 0;
    int interlace = 0;
};

/// Reads the IHDR chunk. False when @p data is not a PNG.
bool read_png_header(const uint8_t* data, size_t size, PngHeader& out);

/// True for what PngRowDecoder handles: 8 bits per channel, not interlaced,
/// grey, RGB, palette, grey+alpha or RGBA, and at most THUMBNAIL_MAX_SIDE a side.
bool png_thumbnail_supported(const PngHeader& header);

/// Palette and transparency chunks, which colour conversion needs.
struct PngPalette {
    uint8_t rgb[256 * 3] = {};
    uint8_t alpha[256] = {};
    int entries = 0;
    bool has_key = false; ///< a tRNS colour key for grey or RGB
    uint8_t key[3] = {};
};

/// Called with each IDAT payload in order; false stops the walk.
using PngIdatSink = bool (*)(const uint8_t* data, size_t size, void* user);

/// Walks the chunks after IHDR, checking each one's length and CRC: fills
/// @p palette from PLTE and tRNS and passes each IDAT payload to @p on_idat.
/// False on a malformed or truncated file, or when @p on_idat returns false.
bool for_each_png_idat(const uint8_t* data, size_t size, PngPalette& palette, PngIdatSink on_idat,
                       void* user);

/// Turns inflated PNG scanline bytes, fed in pieces of any size, into RGBA rows.
class PngRowDecoder {
  public:
    using RowSink = void (*)(const uint8_t* rgba, void* user);

    PngRowDecoder(const PngHeader& header, const PngPalette& palette, RowSink on_row, void* user);

    /// False when a row buffer could not be allocated.
    bool ok() const {
        return cur_ && prev_ && rgba_;
    }
    /// False once the data is invalid (an unknown filter type) or overruns the image.
    bool feed(const uint8_t* data, size_t size);
    bool complete() const {
        return row_ == height_;
    }

  private:
    void finish_row();

    const PngPalette& palette_;
    RowSink on_row_;
    void* user_;
    int width_, height_, color_type_, bpp_;
    size_t stride_;
    std::unique_ptr<uint8_t[]> cur_, prev_, rgba_;
    int filter_ = -1; ///< -1 while the next byte is a row's filter type
    size_t pos_ = 0;
    int row_ = 0;
    bool failed_ = false;
};

/// Least PSRAM a thumbnail decode leaves free. The rest of the app allocates
/// without a fallback, so decodes stop well before it can run out; a decode
/// refused here keeps the placeholder and is tried again later.
inline constexpr size_t THUMBNAIL_PSRAM_FLOOR = 256 * 1024;

/// Working memory a decode takes beside the image it keeps: the inflater state
/// (about 11KB for tinfl), its 32KB window, two rows and the downscale's sums.
inline size_t thumbnail_decode_working_bytes(int src_w, ThumbnailDims dst) {
    return 11 * 1024 + 32 * 1024 + static_cast<size_t>(src_w) * 4 * 3 +
           static_cast<size_t>(dst.w) * 24;
}

/// True when a decode keeping @p kept bytes, and briefly using @p working more,
/// still leaves @p floor of the @p free bytes.
inline bool thumbnail_decode_fits(size_t free, size_t kept, size_t working,
                                  size_t floor = THUMBNAIL_PSRAM_FLOOR) {
    return free >= floor && free - floor >= kept + working;
}

/// Bytes card thumbnails may hold at once, on screen and kept for scrolling
/// back. Opaque thumbnails also hold one card-sized RGB565 backdrop (54KB on
/// an 800x480 screen) outside this budget.
inline constexpr size_t CARD_THUMBNAIL_BUDGET = 960 * 1024;

/// True when one more card thumbnail of @p kept bytes fits the budget beside
/// the @p held bytes card thumbnails hold or have in flight.
inline bool card_thumbnail_fits_budget(size_t held, size_t kept,
                                       size_t budget = CARD_THUMBNAIL_BUDGET) {
    return held <= budget && budget - held >= kept;
}

/// What decode_png_thumbnail() produced.
struct DecodedThumbnail {
    uint8_t* pixels = nullptr; ///< RGB565A8, or RGB565 when opaque; from the Inflate's alloc()
    ThumbnailDims dims;
    bool opaque = false; ///< decoded onto a backdrop: RGB565, no alpha plane
    ThumbnailDecodeFailure failure = ThumbnailDecodeFailure::BadImage;
};

/**
 * @brief Decodes @p png into an RGB565A8 image fitted inside @p max_w x @p max_h.
 *
 * With @p into, the image is written there (it must hold @p into_capacity >=
 * the fitted image) and the decode allocates only its working memory;
 * result.pixels is then @p into, which the caller still owns.
 *
 * With @p backdrop, @p max_w x @p max_h RGB565 pixels, the result is instead an
 * opaque RGB565 image of the whole box: the backdrop with the fitted image
 * centred on it and blended as LVGL would draw it there.
 *
 * @p Inflate wraps a miniz tinfl-compatible streaming inflater (the ESP32 ROM's
 * on the firmware) and the allocator the decode uses:
 *   - `Decompressor` and `static constexpr size_t WINDOW` (32KB for tinfl);
 *   - `static void init(Decompressor*)`;
 *   - `static int step(Decompressor*, const uint8_t* in, size_t* in_size,
 *      uint8_t* window, uint8_t* next, size_t* out_size, bool more_input)`,
 *     returning tinfl's status: 0 done, 1 needs input, 2 more output, <0 failed;
 *   - `static void* alloc(size_t)` returning nullptr on failure, and `free(void*)`;
 *   - `static size_t free_bytes()`, what alloc() has free in total: a decode
 *     that would leave less than THUMBNAIL_PSRAM_FLOOR is not started. It is
 *     not the largest block, because finding that walks the whole heap.
 */
template <class Inflate>
DecodedThumbnail decode_png_thumbnail(const uint8_t* png, size_t size, int max_w, int max_h,
                                      uint8_t* into = nullptr, size_t into_capacity = 0,
                                      const uint16_t* backdrop = nullptr) {
    DecodedThumbnail result;
    PngHeader header;
    if (!read_png_header(png, size, header)) {
        return result;
    }
    if (!png_thumbnail_supported(header)) {
        result.failure = ThumbnailDecodeFailure::Unsupported;
        return result;
    }
    const ThumbnailDims fit = fit_thumbnail(header.width, header.height, max_w, max_h);
    result.opaque = backdrop != nullptr;
    result.dims = result.opaque ? ThumbnailDims{max_w, max_h} : fit;
    const size_t kept = result.opaque ? rgb565_size(result.dims) : rgb565a8_size(result.dims);
    if (into && into_capacity < kept) {
        result.failure = ThumbnailDecodeFailure::Unsupported; // the slot is smaller than the box
        return result;
    }
    if (!thumbnail_decode_fits(Inflate::free_bytes(), into ? 0 : kept,
                               thumbnail_decode_working_bytes(header.width, fit))) {
        result.failure = ThumbnailDecodeFailure::OutOfMemory;
        return result;
    }

    struct Owned {
        void* p = nullptr;
        ~Owned() {
            Inflate::free(p);
        }
    } out, inflater, window;
    out.p = into ? nullptr : Inflate::alloc(kept);
    uint8_t* const pixels = into ? into : static_cast<uint8_t*>(out.p);
    inflater.p = Inflate::alloc(sizeof(typename Inflate::Decompressor));
    window.p = Inflate::alloc(Inflate::WINDOW);
    RowDownscaler* scaler = nullptr;
    if (pixels && result.opaque) {
        std::memcpy(pixels, backdrop, kept);
        // Centred the way LVGL aligns an image inside a larger widget.
        auto* origin = reinterpret_cast<uint16_t*>(pixels) +
                       static_cast<size_t>(max_h / 2 - fit.h / 2) * max_w + (max_w / 2 - fit.w / 2);
        scaler = new (std::nothrow) RowDownscaler(header.width, header.height, fit, origin, max_w);
    } else if (pixels) {
        scaler = new (std::nothrow) RowDownscaler(header.width, header.height, fit, pixels);
    }
    std::unique_ptr<RowDownscaler> scaler_owner(scaler);
    if (!pixels || !inflater.p || !window.p || !scaler || !scaler->ok()) {
        result.failure = ThumbnailDecodeFailure::OutOfMemory;
        return result;
    }

    PngPalette palette;
    PngRowDecoder rows(
        header, palette,
        [](const uint8_t* rgba, void* user) { static_cast<RowDownscaler*>(user)->add_row(rgba); },
        scaler);
    if (!rows.ok()) {
        result.failure = ThumbnailDecodeFailure::OutOfMemory;
        return result;
    }

    struct Stream {
        typename Inflate::Decompressor* d;
        uint8_t* window;
        size_t at = 0;
        int status = 1;
        PngRowDecoder* rows;

        // Inflates @p in (or, with more_input false, the end of the stream)
        // through the circular window into the row decoder.
        bool inflate(const uint8_t* in, size_t left, bool more_input) {
            for (;;) {
                size_t in_size = left;
                size_t out_size = Inflate::WINDOW - at;
                status = Inflate::step(d, in, &in_size, window, window + at, &out_size, more_input);
                in += in_size;
                left -= in_size;
                if (out_size && !rows->feed(window + at, out_size)) {
                    return false;
                }
                at = (at + out_size) & (Inflate::WINDOW - 1);
                if (status < 0) {
                    return false;
                }
                if (status == 0 || (status == 1 && left == 0)) {
                    return true;
                }
            }
        }
    } stream{static_cast<typename Inflate::Decompressor*>(inflater.p),
             static_cast<uint8_t*>(window.p), 0, 1, &rows};
    Inflate::init(stream.d);

    const bool walked = for_each_png_idat(
        png, size, palette,
        [](const uint8_t* data, size_t len, void* user) {
            auto* s = static_cast<Stream*>(user);
            return s->status == 0 || s->inflate(data, len, true);
        },
        &stream);
    if (!walked || (stream.status != 0 && !stream.inflate(nullptr, 0, false)) || !rows.complete()) {
        return result;
    }
    result.failure = ThumbnailDecodeFailure::None;
    result.pixels = pixels;
    out.p = nullptr;
    return result;
}

} // namespace helix
