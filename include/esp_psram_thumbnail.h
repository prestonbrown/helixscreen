// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// ESP32-only. PSRAM-resident thumbnail for the print-select cards
// and the print status panel. LittleFS is too small for a disk thumbnail cache
// on this platform (see thumbnail_cache.cpp), so a
// thumbnail fetched via download_file_partial is decoded once, fitted to the
// size it is drawn at, and kept as an RGB565A8 lv_image_dsc_t in PSRAM: the
// form a prescaled .bin takes elsewhere. Drawing the PNG itself would hold a
// full-size ARGB8888 decode per image in LVGL's image cache, which PSRAM
// cannot keep for more than one.
#if defined(HELIX_PLATFORM_ESP32)

#include "async_lifetime_guard.h" // for helix::internal::on_main_thread()
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "miniz.h" // the ROM's streaming inflater
#include "thumbnail_downscale.h"
#include "thumbnail_png_stream.h"
#include "thumbnail_slot_pool.h"

#include <lvgl/src/misc/cache/instance/lv_image_cache.h> // lv_image_cache_drop()

#include <memory>
#include <new>
#include <string>

namespace helix::ui {

/**
 * @brief Owns one PSRAM-allocated decoded image plus the lv_image_dsc_t
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
            // file's thumbnail. Drop it explicitly.
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
            if (slots_) {
                slots_->release(data_);
            } else {
                heap_caps_free(data_);
            }
        }
    }

    /// Decodes png_bytes once and keeps it as an RGB565A8 image fitted inside
    /// max_w x max_h, so drawing it needs no PNG decoder and no image-cache
    /// entry. The PNG streams through the ROM inflater a row at a time into the
    /// downscale (decode_png_thumbnail), so the full-size image never exists and
    /// nothing is reserved: a decode allocates the inflater state, its 32KB
    /// window, two rows and the kept image (3 bytes per kept pixel). Touches no
    /// widget state, so it is safe on the HTTP lane worker. Returns nullptr when
    /// nothing was produced, and says why in @p failure.
    static std::shared_ptr<EspPsramThumbnail>
    create_decoded(const std::string& png_bytes, int max_w, int max_h,
                   helix::ThumbnailDecodeFailure& failure) {
        const helix::DecodedThumbnail decoded = helix::decode_png_thumbnail<RomInflate>(
            reinterpret_cast<const uint8_t*>(png_bytes.data()), png_bytes.size(), max_w, max_h);
        failure = decoded.failure;
        if (!decoded.pixels) {
            return nullptr;
        }
        auto* thumb = new (std::nothrow) EspPsramThumbnail(decoded.pixels, decoded);
        if (!thumb) {
            RomInflate::free(decoded.pixels);
            failure = helix::ThumbnailDecodeFailure::OutOfMemory;
            return nullptr;
        }
        return std::shared_ptr<EspPsramThumbnail>(thumb);
    }

    /// The same decode, written into a slot of @p slots, so a card that scrolls
    /// away and another that scrolls in reuse one buffer rather than freeing and
    /// allocating one. The slot goes back to @p slots when the thumbnail goes.
    /// With @p backdrop (max_w x max_h RGB565, what the image is drawn over),
    /// the image is opaque RGB565 of the whole box and draws as a plain copy.
    /// Safe on the HTTP lane worker: the pool locks.
    static std::shared_ptr<EspPsramThumbnail>
    create_decoded(const std::string& png_bytes, int max_w, int max_h,
                   const std::shared_ptr<helix::ThumbnailSlotPool>& slots,
                   helix::ThumbnailDecodeFailure& failure, const uint16_t* backdrop = nullptr) {
        uint8_t* slot = slots ? slots->acquire() : nullptr;
        if (!slot) {
            failure = helix::ThumbnailDecodeFailure::OutOfMemory;
            return nullptr;
        }
        const helix::DecodedThumbnail decoded = helix::decode_png_thumbnail<RomInflate>(
            reinterpret_cast<const uint8_t*>(png_bytes.data()), png_bytes.size(), max_w, max_h,
            slot, slots->slot_bytes(), backdrop);
        failure = decoded.failure;
        auto* thumb =
            decoded.pixels ? new (std::nothrow) EspPsramThumbnail(slot, decoded) : nullptr;
        if (!thumb) {
            slots->release(slot);
            if (decoded.pixels) {
                failure = helix::ThumbnailDecodeFailure::OutOfMemory;
            }
            return nullptr;
        }
        thumb->slots_ = slots;
        return std::shared_ptr<EspPsramThumbnail>(thumb);
    }

    /// Bytes the kept image holds.
    size_t bytes() const {
        return dsc_.data_size;
    }

    /// Pointer suitable for lv_image_set_src().
    const lv_image_dsc_t* dsc() const {
        return &dsc_;
    }

  private:
    /// The ROM's tinfl and PSRAM, for decode_png_thumbnail().
    struct RomInflate {
        using Decompressor = tinfl_decompressor;
        static constexpr size_t WINDOW = TINFL_LZ_DICT_SIZE;
        static void init(Decompressor* d) {
            tinfl_init(d);
        }
        static int step(Decompressor* d, const uint8_t* in, size_t* in_size, uint8_t* window,
                        uint8_t* next, size_t* out_size, bool more_input) {
            return tinfl_decompress(d, in, in_size, window, next, out_size,
                                    TINFL_FLAG_PARSE_ZLIB_HEADER |
                                        (more_input ? TINFL_FLAG_HAS_MORE_INPUT : 0));
        }
        static void* alloc(size_t size) {
            return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
        }
        static void free(void* p) {
            heap_caps_free(p);
        }
        // Not heap_caps_get_largest_free_block(): it walks every PSRAM block
        // with interrupts masked for 20-30ms, far longer than the panel's
        // bounce-buffer refill can wait, so every call glitches the screen.
        static size_t free_bytes() {
            return heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        }
    };

    EspPsramThumbnail(uint8_t* data, const helix::DecodedThumbnail& decoded) : data_(data) {
        const helix::ThumbnailDims dims = decoded.dims;
        dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc_.header.cf = decoded.opaque ? LV_COLOR_FORMAT_RGB565 : LV_COLOR_FORMAT_RGB565A8;
        dsc_.header.w = static_cast<uint32_t>(dims.w);
        dsc_.header.h = static_cast<uint32_t>(dims.h);
        dsc_.header.stride = static_cast<uint32_t>(dims.w * 2); // the RGB565 plane's
        dsc_.data_size = static_cast<uint32_t>(decoded.opaque ? helix::rgb565_size(dims)
                                                              : helix::rgb565a8_size(dims));
        dsc_.data = data_;
    }

    uint8_t* data_ = nullptr;
    std::shared_ptr<helix::ThumbnailSlotPool> slots_; ///< owns data_ when set
    lv_image_dsc_t dsc_{};
};

} // namespace helix::ui

#endif // HELIX_PLATFORM_ESP32
