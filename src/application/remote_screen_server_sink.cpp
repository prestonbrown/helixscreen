// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "remote_screen_server_sink.h"

#include "bitmap_font_5x7.h"
#include "jpeg_encode.h"
#include "memory_utils.h"
#include "remote_screen_blit.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <thread>
#include <utility>

namespace helix {

SnapshotRateCap snapshot_rate_cap(bool constrained_host) {
    return constrained_host ? SnapshotRateCap{5, 1} : SnapshotRateCap{5, 2};
}

bool remote_screen_host_is_constrained() {
    return get_system_memory_info().is_constrained_device() ||
           std::thread::hardware_concurrency() <= 1;
}

std::chrono::milliseconds min_encode_interval(SnapshotRateCap cap, bool printing) {
    const int fps = printing ? cap.printing_fps : cap.idle_fps;
    return std::chrono::milliseconds(1000 / (fps > 0 ? fps : 1));
}

void draw_screen_placeholder_rgb888(int w, int h, std::vector<uint8_t>& rgb) {
    constexpr uint8_t kBackground = 0x20;
    constexpr uint8_t kText = 0xC0;
    rgb.assign(static_cast<size_t>(std::max(w, 0)) * std::max(h, 0) * 3, kBackground);
    if (w <= 0 || h <= 0) {
        return;
    }
    // The largest scale whose line fills at most 80% of the width.
    const int unit_w = font5x7_text_width(kScreenHiddenMessage, 1);
    const int scale = std::min(w * 8 / 10 / unit_w, h / (kFont5x7H * 4));
    if (scale < 1) {
        return;
    }
    const int x0 = (w - font5x7_text_width(kScreenHiddenMessage, scale)) / 2;
    const int y0 = (h - kFont5x7H * scale) / 2;
    font5x7_draw_text(kScreenHiddenMessage, x0, y0, scale, [&](int x, int y) {
        if (x >= 0 && x < w && y >= 0 && y < h) {
            std::memset(&rgb[(static_cast<size_t>(y) * w + x) * 3], kText, 3);
        }
    });
}

// ---------------------------------------------------------------------------
// ScreenFrameStore
// ---------------------------------------------------------------------------

ScreenFrameStore::ScreenFrameStore(Clock clock) : clock_(std::move(clock)) {}

std::chrono::steady_clock::time_point ScreenFrameStore::now() const {
    return clock_ ? clock_() : std::chrono::steady_clock::now();
}

bool ScreenFrameStore::within_viewer_idle(int64_t last) const {
    if (last == INT64_MIN) {
        return false;
    }
    const auto idle = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::milliseconds(kViewerIdleMs));
    return now().time_since_epoch().count() - last < idle.count();
}

bool ScreenFrameStore::wants_frames() const {
    return within_viewer_idle(last_request_.load(std::memory_order_acquire));
}

void ScreenFrameStore::note_page_view() {
    last_page_view_.store(now().time_since_epoch().count(), std::memory_order_release);
}

bool ScreenFrameStore::page_viewer_present() const {
    return within_viewer_idle(last_page_view_.load(std::memory_order_acquire));
}

void ScreenFrameStore::on_frame(const RemoteScreenFrame& f) {
    const int bpp = remote_frame_src_bpp(f.src_format);
    if (bpp == 0 || f.disp_w <= 0 || f.disp_h <= 0) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (f.disp_w != shadow_w_ || f.disp_h != shadow_h_ || bpp != shadow_bpp_) {
            const bool resized = !shadow_.empty();
            shadow_.assign(static_cast<size_t>(f.disp_w) * f.disp_h * bpp, 0);
            shadow_w_ = f.disp_w;
            shadow_h_ = f.disp_h;
            shadow_bpp_ = bpp;
            if (resized) {
                full_redraw_needed_ = true;
            }
        }
        if (f.secret != ScreenSecret::Absent) {
            clean_ = false;
            clean_pending_ = false;
        } else if (!clean_ && !clean_pending_) {
            // Pixels of the secret may sit anywhere in the shadow until a full repaint
            // drawn without it has landed.
            clean_pending_ = true;
            clean_after_full_ = full_generation_;
            if (!awaiting_full_) {
                full_redraw_needed_ = true;
            }
        }
        const BlitResult r = blit_remote_frame(f, BlitTarget{shadow_.data(), shadow_.size(),
                                                             static_cast<uint32_t>(shadow_w_ * bpp),
                                                             shadow_w_, shadow_h_, bpp * 8});
        if (diag_frames_ < kDiagFrames) {
            ++diag_frames_;
            spdlog::debug("[RemoteScreen] frame #{} rect=({},{})-({},{}) origin=({},{}) disp={}x{} "
                          "stride={} len={} bpp={} -> status {}",
                          diag_frames_, f.x1, f.y1, f.x2, f.y2, f.px_map_x, f.px_map_y, f.disp_w,
                          f.disp_h, f.src_stride, static_cast<unsigned long>(f.px_map_len), bpp,
                          static_cast<int>(r.status));
        }
        if (r.status != BlitStatus::Copied) {
            if (!reject_warned_) {
                reject_warned_ = true;
                spdlog::warn("[RemoteScreen] dropping a frame rect=({},{})-({},{}) origin=({},{}) "
                             "disp={}x{} stride={} len={}: blit status {}",
                             f.x1, f.y1, f.x2, f.y2, f.px_map_x, f.px_map_y, f.disp_w, f.disp_h,
                             f.src_stride, static_cast<unsigned long>(f.px_map_len),
                             static_cast<int>(r.status));
            }
            // A refresh whose other rects landed is still the screen, less this rect.
            if (!(f.last_in_refresh && refresh_copied_)) {
                return;
            }
        } else {
            refresh_copied_ = true;
        }
        if (!f.last_in_refresh) {
            return;
        }
        refresh_copied_ = false;
        has_frame_ = true;
        ++generation_;
        if (awaiting_full_) {
            awaiting_full_ = false;
            ++full_generation_;
        }
        if (clean_pending_ && full_generation_ > clean_after_full_) {
            clean_pending_ = false;
            clean_ = true;
        }
    }
    cv_.notify_all();
}

ScreenFrameStore::Need ScreenFrameStore::note_request() {
    last_request_.store(now().time_since_epoch().count(), std::memory_order_release);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_frame_ || full_redraw_needed_) {
        full_redraw_needed_ = false;
        return Need::FullRedraw;
    }
    return Need::Ready;
}

void ScreenFrameStore::note_full_invalidate() {
    std::lock_guard<std::mutex> lock(mutex_);
    awaiting_full_ = true;
}

uint64_t ScreenFrameStore::full_generation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return full_generation_;
}

bool ScreenFrameStore::wait_for_full_refresh_after(uint64_t full_gen,
                                                   std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    const uint64_t epoch = abort_epoch_;
    cv_.wait_for(lock, timeout,
                 [&] { return full_generation_ > full_gen || abort_epoch_ != epoch; });
    return full_generation_ > full_gen && abort_epoch_ == epoch;
}

void ScreenFrameStore::abort_waits() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++abort_epoch_;
    }
    cv_.notify_all();
}

std::pair<int, int> ScreenFrameStore::frame_size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return has_frame_ ? std::pair<int, int>{shadow_w_, shadow_h_} : std::pair<int, int>{0, 0};
}

uint64_t ScreenFrameStore::generation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
}

std::optional<EncodedSnapshot> ScreenFrameStore::snapshot(bool printing, SnapshotRateCap cap) {
    std::lock_guard<std::mutex> encode_lock(encode_mutex_);
    const auto t = now();
    uint64_t gen = 0;
    int w = 0;
    int h = 0;
    bool placeholder = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!has_frame_) {
            return std::nullopt;
        }
        w = shadow_w_;
        h = shadow_h_;
        gen = generation_;
        placeholder = !clean_;
        if (placeholder) {
            if (placeholder_jpeg_ && placeholder_w_ == w && placeholder_h_ == h) {
                return EncodedSnapshot{placeholder_jpeg_, gen, w, h};
            }
        } else {
            if (cached_jpeg_ && (cached_generation_ == generation_ ||
                                 t - last_encode_ < min_encode_interval(cap, printing))) {
                return EncodedSnapshot{cached_jpeg_, cached_generation_, cached_w_, cached_h_};
            }
            rgb_.resize(static_cast<size_t>(w) * h * 3);
            convert_to_rgb888(shadow_.data(), static_cast<uint32_t>(w * shadow_bpp_), w, h,
                              shadow_bpp_, rgb_.data());
        }
    }

    // The placeholder is drawn from nothing but its size, so no screen pixel reaches it.
    if (placeholder) {
        draw_screen_placeholder_rgb888(w, h, rgb_);
    }
    auto jpeg = std::make_shared<std::vector<uint8_t>>();
    if (!encode_jpeg_rgb888(rgb_.data(), w, h, kRemoteScreenJpegQuality, *jpeg)) {
        spdlog::warn("[RemoteScreen] JPEG encode failed ({}x{})", w, h);
        return std::nullopt;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (placeholder) {
        placeholder_jpeg_ = std::move(jpeg);
        placeholder_w_ = w;
        placeholder_h_ = h;
        return EncodedSnapshot{placeholder_jpeg_, gen, w, h};
    }
    cached_jpeg_ = std::move(jpeg);
    cached_generation_ = gen;
    cached_w_ = w;
    cached_h_ = h;
    last_encode_ = t;
    return EncodedSnapshot{cached_jpeg_, gen, w, h};
}

bool ScreenFrameStore::screen_clean() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return clean_;
}

void ScreenFrameStore::release_locked() {
    std::vector<uint8_t>().swap(shadow_);
    std::vector<uint8_t>().swap(rgb_);
    cached_jpeg_.reset();
    placeholder_jpeg_.reset();
    placeholder_w_ = placeholder_h_ = 0;
    clean_ = true;
    clean_pending_ = false;
    shadow_w_ = shadow_h_ = shadow_bpp_ = 0;
    cached_w_ = cached_h_ = 0;
    has_frame_ = false;
    full_redraw_needed_ = true;
}

bool ScreenFrameStore::release_if_idle() {
    if (wants_frames()) {
        return false;
    }
    std::lock_guard<std::mutex> encode_lock(encode_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    const bool had = !shadow_.empty() || !rgb_.empty() || cached_jpeg_ || placeholder_jpeg_;
    release_locked();
    return had;
}

void ScreenFrameStore::release() {
    last_request_.store(INT64_MIN, std::memory_order_release);
    last_page_view_.store(INT64_MIN, std::memory_order_release);
    std::lock_guard<std::mutex> encode_lock(encode_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    release_locked();
}

// ---------------------------------------------------------------------------
// ScreenServerSink
// ---------------------------------------------------------------------------

ScreenServerSink::ScreenServerSink(std::shared_ptr<ScreenFrameStore> store)
    : store_(std::move(store)) {}

bool ScreenServerSink::start() {
    return store_ != nullptr;
}

void ScreenServerSink::stop() {
    if (store_) {
        store_->release();
    }
}

bool ScreenServerSink::wants_frames() const {
    return store_ && store_->wants_frames();
}

void ScreenServerSink::on_frame(const RemoteScreenFrame& frame) {
    store_->on_frame(frame);
}

const char* ScreenServerSink::name() const {
    return "ScreenServerSink"; // i18n: do not translate - internal sink identifier
}

} // namespace helix
