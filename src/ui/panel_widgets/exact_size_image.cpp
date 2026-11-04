// SPDX-License-Identifier: GPL-3.0-or-later

#include "exact_size_image.h"

#include "core/http_executor.h"
#include "helix_fs.h"
#include "prerendered_images.h"

#include <spdlog/spdlog.h>

#include <utility>

namespace helix::ui {

namespace {

/// The size the image takes at the next layout pass. A callout move declares a
/// pixel rect whose coords only follow once layout runs, and this is read from
/// timers that may fire before it, so the declared size wins over the coords;
/// a 100% resolves against the parent's content box.
void declared_image_size(lv_obj_t* img, int32_t& w, int32_t& h) {
    lv_obj_t* parent = lv_obj_get_parent(img);
    const auto resolve = [](int32_t v, int32_t full) -> int32_t {
        if (LV_COORD_IS_PCT(v))
            return full * LV_COORD_GET_PCT(v) / 100;
        // LV_SIZE_CONTENT and the other special values carry no pixel size.
        return LV_COORD_IS_SPEC(v) ? 0 : v;
    };
    w = resolve(lv_obj_get_style_width(img, LV_PART_MAIN), lv_obj_get_content_width(parent));
    h = resolve(lv_obj_get_style_height(img, LV_PART_MAIN), lv_obj_get_content_height(parent));
}

} // namespace

ExactSizeImage::ExactSizeImage(const char* log_tag, ImageLookup image, SetSource set_src)
    : log_tag_(log_tag), image_(std::move(image)), set_src_(std::move(set_src)) {}

void ExactSizeImage::show_exact(lv_obj_t* img, const std::string& lvgl_path) {
    // LVGL keeps the scale CONTAIN computed when the align leaves CONTAIN
    // (lv_image_set_scale refuses while CONTAIN is still set), so the scale is
    // reset after the align changes, and before the src so no CONTAIN pass runs
    // on the new copy.
    // DECLARATIVE_OK: cached asset choice from measured size
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);
    // DECLARATIVE_OK: cached asset choice from measured size
    lv_image_set_scale(img, LV_SCALE_NONE);
    set_src_(img, lvgl_path);
    displayed_path_ = lvgl_path;
}

bool ExactSizeImage::show_cached(const std::string& source) {
    source_ = source;
    lv_obj_t* img = image_ ? image_() : nullptr;
    if (!img || source.empty())
        return false;

    int32_t w = 0, h = 0;
    declared_image_size(img, w, h);
    if (w <= 0 || h <= 0)
        return false;

    const std::string cache_path = helix::get_cached_printer_image_path(source, w, h);
    // Any stat failure reads as "not found": the ESP32 VFS reports missing
    // paths as ENODATA, which must not surface as an error.
    if (!helix::fs::exists(cache_path))
        return false;

    if (source == applied_source_ && w == applied_w_ && h == applied_h_)
        applied_exact_ = true;
    const std::string lvgl_path = "A:" + cache_path;
    if (lvgl_path == displayed_path_)
        return true; // already showing it; re-setting would invalidate for nothing

    show_exact(img, lvgl_path);
    spdlog::debug("[{}] Exact-size image: {} ({}x{})", log_tag_, cache_path, w, h);
    return true;
}

void ExactSizeImage::show_scaled(const std::string& source) {
    source_ = source;
    lv_obj_t* img = image_ ? image_() : nullptr;
    if (!img || source.empty())
        return;
    set_src_(img, source);
    // DECLARATIVE_OK: cached asset choice from measured size
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CONTAIN);
    displayed_path_ = source;
    spdlog::debug("[{}] Source image: '{}'", log_tag_, source);
}

void ExactSizeImage::generate_if_missing(const std::string& source) {
    if (show_cached(source))
        return;
    lv_obj_t* img = image_ ? image_() : nullptr;
    if (!img || source.empty())
        return;

    int32_t w = 0, h = 0;
    declared_image_size(img, w, h);
    if (w <= 0 || h <= 0) {
        spdlog::debug("[{}] Not laid out yet ({}x{}), skipping cache", log_tag_, w, h);
        return;
    }
    // Decoding and resizing an image is half a second per entry on a two-core
    // MIPS board, so it runs off the UI thread.
    if (inflight_) {
        spdlog::debug("[{}] Cache generation already running, skipping {}x{}", log_tag_, w, h);
        return;
    }

    // Idempotent; covers unit tests and any call site reached before Application
    // starts the pools.
    helix::http::HttpExecutor::fast().start();
    if (!helix::http::HttpExecutor::fast().running()) {
        // No worker pool on this build (the ESP32 firmware): keep the scaled
        // source, the same state a generation failure leaves.
        spdlog::debug("[{}] No background worker, using scaled source", log_tag_);
        return;
    }

    const std::string cache_path = helix::get_cached_printer_image_path(source, w, h);
    spdlog::debug("[{}] Cache miss, generating {}x{} from '{}'", log_tag_, w, h, source);
    inflight_ = true;
    auto tok = lifetime_.token();
    const int gen_w = static_cast<int>(w);
    const int gen_h = static_cast<int>(h);

    helix::http::HttpExecutor::fast().submit([this, tok, source, cache_path, gen_w, gen_h]() {
        // Worker thread: no `this` access and no lv_* call. The generation reads
        // and writes files and returns a plain bool.
        const bool generated =
            helix::generate_cached_printer_image(source, gen_w, gen_h, cache_path);

        tok.defer("ExactSizeImage::cache_generated", [this, source, cache_path, gen_w, gen_h,
                                                      generated]() {
            inflight_ = false;
            if (source_ != source) {
                // The owner moved to another image while the worker ran, and its
                // own request was refused while this one ran. The entry stays on
                // disk for whenever this source comes back.
                spdlog::debug("[{}] Cache for '{}' is no longer the displayed source, "
                              "discarding result",
                              log_tag_, source);
                if (!source_.empty())
                    generate_if_missing(source_);
                return;
            }
            if (!generated) {
                spdlog::warn("[{}] Cache generation failed, using scaled source", log_tag_);
                return;
            }
            lv_obj_t* img = image_ ? image_() : nullptr;
            if (!img)
                return;
            int32_t now_w = 0, now_h = 0;
            declared_image_size(img, now_w, now_h);
            if (now_w != gen_w || now_h != gen_h) {
                // The image was resized while the worker ran; this copy is cut
                // for the old rect.
                generate_if_missing(source);
                return;
            }
            show_exact(img, "A:" + cache_path);
            if (applied_source_ == source && applied_w_ == now_w && applied_h_ == now_h)
                applied_exact_ = true;
            spdlog::debug("[{}] Cached and loaded: {} ({}x{})", log_tag_, cache_path, gen_w, gen_h);
        });
    });
}

bool ExactSizeImage::apply(const std::string& source) {
    lv_obj_t* img = image_ ? image_() : nullptr;
    if (!img || source.empty())
        return false;
    int32_t w = 0, h = 0;
    declared_image_size(img, w, h);
    if (source == applied_source_ && w == applied_w_ && h == applied_h_)
        return applied_exact_;
    applied_source_ = source;
    applied_w_ = w;
    applied_h_ = h;
    applied_exact_ = show_cached(source);
    if (!applied_exact_) {
        show_scaled(source);
        generate_if_missing(source);
    }
    return applied_exact_;
}

void ExactSizeImage::forget_displayed() {
    displayed_path_.clear();
    applied_source_.clear();
    applied_exact_ = false;
}

void ExactSizeImage::cancel() {
    lifetime_.invalidate();
    inflight_ = false;
}

} // namespace helix::ui
