// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// ESP32-only (Task 11 R2). PSRAM-resident PNG thumbnail wrapper for the
// print-select card view. LittleFS is too small for a disk thumbnail cache
// on this platform (Task 10 R6 hard gate — see thumbnail_cache.cpp), so
// thumbnails fetched via download_file_partial are decoded straight into an
// lv_image_dsc_t backed by a PSRAM buffer instead of a cache file. LVGL's
// lodepng decoder (LV_USE_LODEPNG, enabled in the ESP32 lv_conf.h override)
// decodes the raw PNG bytes on demand at draw time — lv_image_src_get_type()
// auto-detects an lv_image_dsc_t* via its header.magic byte, so no manual
// RGB/ARGB pre-decode or separate API call is needed here. create_decoded()
// is the other form: it decodes once up front and keeps a small RGB565A8
// image, for a thumbnail drawn at one known size where a full-size ARGB8888
// decode per draw would not fit.
#if defined(HELIX_PLATFORM_ESP32)

#include "async_lifetime_guard.h" // for helix::internal::on_main_thread()
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "thumbnail_downscale.h"

// The header's C++ overloads sit inside its extern "C" block and do not
// compile from C++; only the C API is used here.
#ifndef LODEPNG_NO_COMPILE_CPP
#define LODEPNG_NO_COMPILE_CPP
#endif
#include <lvgl/src/libs/lodepng/lodepng.h>
#include <lvgl/src/misc/cache/instance/lv_image_cache.h> // lv_image_cache_drop()

#include <cstring>
#include <memory>
#include <string>

namespace helix::ui {

/**
 * @brief Owns one PSRAM-allocated PNG byte buffer plus the lv_image_dsc_t
 *        wrapping it, for a single print-file thumbnail.
 *
 * Instances are shared_ptr-managed and held both by the PrintFileData entry
 * (source of truth) and by the card widget slot currently displaying it
 * (CardWidgetData::esp_thumbnail) so the buffer stays alive for as long as
 * any widget's `src` still points at its descriptor, independent of when the
 * owning PrintFileData is replaced during a list refresh/sort.
 */
class EspPsramThumbnail {
  public:
    EspPsramThumbnail(const EspPsramThumbnail&) = delete;
    EspPsramThumbnail& operator=(const EspPsramThumbnail&) = delete;

    ~EspPsramThumbnail() {
        if (data_) {
            // LVGL's image cache keys a variable-source (lv_image_dsc_t*) entry
            // on the source pointer itself (&dsc_ here). If this buffer's heap
            // address gets reused for a later EspPsramThumbnail and the old
            // cache entry hasn't LRU-evicted yet, lv_image_set_src(new dsc)
            // could hit the stale decoded bitmap — a card showing a previous
            // file's thumbnail (review Focus 2). Drop it explicitly.
            //
            // lv_image_cache_drop() reaches into the draw units
            // (LV_EVENT_INVALIDATE_AREA broadcast) and is documented unsafe
            // off the UI thread (see Application's memory-pressure responder,
            // application.cpp). Every normal destruction path here (file_list_
            // replaced/sorted, card recycled, panel torn down) runs on the
            // main thread via UpdateQueue::process_pending() — the shared_ptr
            // is only ever unwrapped inside a tok.defer()'d lambda, and
            // UpdateQueue::queue() always stores that lambda into
            // pending_/frozen_buffer_ to run there. The ONE exception:
            // queue()'s shut_down_ branch drops the incoming callback (and
            // whatever it captured) synchronously on the CALLING thread,
            // which could be the EspHttpLane worker if a fetch completes
            // after UpdateQueue::shutdown() has already run. Guard instead of
            // assuming that race can't happen; skipping the drop there is
            // harmless since the process is already tearing down.
            if (helix::internal::on_main_thread()) {
                lv_image_cache_drop(dsc());
            }
            heap_caps_free(data_);
        }
    }

    /// Copies png_bytes into a fresh PSRAM allocation and builds the
    /// lv_image_dsc_t wrapper. Returns nullptr if the PSRAM allocation fails
    /// — never falls back to internal RAM (Task 11 R2 hard constraint).
    static std::shared_ptr<EspPsramThumbnail> create(const std::string& png_bytes) {
        if (png_bytes.empty()) {
            return nullptr;
        }
        auto* buf = static_cast<uint8_t*>(heap_caps_malloc(png_bytes.size(), MALLOC_CAP_SPIRAM));
        if (!buf) {
            return nullptr;
        }
        memcpy(buf, png_bytes.data(), png_bytes.size());
        // Private ctor blocks make_shared; a single small control-block alloc
        // per thumbnail fetch is not perf sensitive (not per-frame).
        return std::shared_ptr<EspPsramThumbnail>(new EspPsramThumbnail(buf, png_bytes.size()));
    }

    /// Decodes png_bytes once and keeps it as an RGB565A8 image fitted inside
    /// max_w x max_h, so drawing it needs no PNG decoder and no image-cache
    /// entry. A 300px PNG costs ~720KB only while it decodes here, then 3 bytes
    /// per kept pixel. Safe on the HTTP lane worker: lodepng allocates through
    /// lv_malloc (the C library allocator on this build) and touches no widget
    /// state. Returns nullptr when the PNG cannot be decoded or memory runs out,
    /// and says which in @p failure.
    static std::shared_ptr<EspPsramThumbnail>
    create_decoded(const std::string& png_bytes, int max_w, int max_h,
                   helix::ThumbnailDecodeFailure& failure) {
        failure = helix::ThumbnailDecodeFailure::None;
        if (png_bytes.empty()) {
            failure = helix::ThumbnailDecodeFailure::BadImage;
            return nullptr;
        }
        unsigned char* decoded_raw = nullptr;
        unsigned w = 0;
        unsigned h = 0;
        const unsigned err = lodepng_decode32(
            &decoded_raw, &w, &h, reinterpret_cast<const unsigned char*>(png_bytes.data()),
            png_bytes.size());
        // This lodepng port hands back an lv_draw_buf_t, not a bare pixel array.
        auto* decoded = reinterpret_cast<lv_draw_buf_t*>(decoded_raw);
        if (err || !decoded) {
            if (decoded) {
                lv_draw_buf_destroy(decoded);
            }
            failure =
                err ? helix::classify_lodepng_error(err) : helix::ThumbnailDecodeFailure::BadImage;
            return nullptr;
        }
        const helix::ThumbnailDims dims =
            helix::fit_thumbnail(static_cast<int>(w), static_cast<int>(h), max_w, max_h);
        const size_t size = helix::rgb565a8_size(dims);
        auto* buf =
            size ? static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM)) : nullptr;
        if (buf) {
            helix::downscale_rgba_to_rgb565a8(decoded->data, static_cast<int>(w),
                                              static_cast<int>(h), dims, buf);
        }
        lv_draw_buf_destroy(decoded);
        if (!buf) {
            failure = size ? helix::ThumbnailDecodeFailure::OutOfMemory
                           : helix::ThumbnailDecodeFailure::BadImage;
            return nullptr;
        }
        return std::shared_ptr<EspPsramThumbnail>(new EspPsramThumbnail(buf, size, dims));
    }

    /// Pointer suitable for lv_image_set_src().
    const lv_image_dsc_t* dsc() const {
        return &dsc_;
    }

  private:
    EspPsramThumbnail(uint8_t* data, size_t size) : data_(data) {
        dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc_.header.cf = LV_COLOR_FORMAT_RAW;
        dsc_.header.flags = LV_IMAGE_FLAGS_COMPRESSED;
        dsc_.header.w = 0;
        dsc_.header.h = 0;
        dsc_.data_size = static_cast<uint32_t>(size);
        dsc_.data = data_;
    }

    EspPsramThumbnail(uint8_t* data, size_t size, helix::ThumbnailDims dims) : data_(data) {
        dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc_.header.cf = LV_COLOR_FORMAT_RGB565A8;
        dsc_.header.w = static_cast<uint32_t>(dims.w);
        dsc_.header.h = static_cast<uint32_t>(dims.h);
        dsc_.header.stride = static_cast<uint32_t>(dims.w * 2); // the RGB565 plane's
        dsc_.data_size = static_cast<uint32_t>(size);
        dsc_.data = data_;
    }

    uint8_t* data_ = nullptr;
    lv_image_dsc_t dsc_{};
};

} // namespace helix::ui

#endif // HELIX_PLATFORM_ESP32
