// SPDX-License-Identifier: GPL-3.0-or-later

#include "snapshot_qr_scanner.h"

#include "hv/requests.h"
#include "stb_image.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>

namespace helix {

SnapshotQrScanner::SnapshotQrScanner() = default;

SnapshotQrScanner::~SnapshotQrScanner() {
    stop();
}

void SnapshotQrScanner::start(const std::string& snapshot_url, FrameCallback on_frame,
                              QrResultCallback on_qr_result, ErrorCallback on_error) {
    if (running_.load()) {
        spdlog::warn("[SnapshotQR] Already running");
        return;
    }

    snapshot_url_ = snapshot_url;
    on_frame_ = std::move(on_frame);
    on_qr_result_ = std::move(on_qr_result);
    on_error_ = std::move(on_error);
    running_ = true;
    frame_pending_ = false;

    // Wrap — EAGAIN under thread exhaustion throws std::system_error ([L083]).
    try {
        poll_thread_ = std::thread([this]() { poll_loop(); });
        spdlog::info("[SnapshotQR] Started polling {}", snapshot_url_);
    } catch (const std::system_error& e) {
        spdlog::error("[SnapshotQR] Failed to spawn poll thread: {}", e.what());
        running_ = false;
        if (on_error_) {
            on_error_("system busy");
        }
    }
}

void SnapshotQrScanner::stop() {
    bool was_running = running_.exchange(false);
    lifetime_.invalidate();

    // Always join if the thread is joinable (even if running_ was already false —
    // the poll thread may have self-stopped after a successful QR decode)
    if (poll_thread_.joinable()) {
        poll_thread_.join();
    }

    if (was_running || display_buf_ || decode_buf_) {
        free_frame_bufs();
        grayscale_buf_.clear();
        grayscale_buf_.shrink_to_fit();

        on_frame_ = nullptr;
        on_qr_result_ = nullptr;
        on_error_ = nullptr;

        spdlog::info("[SnapshotQR] Stopped and freed all buffers");
    }
}

void SnapshotQrScanner::frame_consumed() {
    frame_pending_ = false;
}

void SnapshotQrScanner::poll_loop() {
    int backoff_ms = 0;

    spdlog::debug("[SnapshotQR] Poll loop started (interval={}ms)", POLL_INTERVAL_MS);

    // poll_thread_ is dtor-joined by stop(), which sets running_ = false
    // before joining. running_ is the only exit signal we need — using
    // lifetime_.token() on a bg thread would trip the L081 runtime detector
    // even though this thread is dtor-joined, because the detector matches
    // on program counter, not on join semantics.
    while (running_.load()) {
        if (!frame_pending_.load()) {
            if (fetch_and_decode()) {
                backoff_ms = 0;
            } else {
                backoff_ms = std::min(backoff_ms + 1000, MAX_BACKOFF_MS);
            }
        }

        // Sleep in small increments so we can exit quickly
        int sleep_ms = backoff_ms > 0 ? backoff_ms : POLL_INTERVAL_MS;
        for (int elapsed = 0; elapsed < sleep_ms && running_.load(); elapsed += POLL_STEP_MS) {
            std::this_thread::sleep_for(std::chrono::milliseconds(POLL_STEP_MS));
        }
    }

    spdlog::debug("[SnapshotQR] Poll loop exited");
}

bool SnapshotQrScanner::fetch_and_decode() {
    if (snapshot_url_.empty())
        return false;

    spdlog::info("[SnapshotQR] Fetching snapshot from {}", snapshot_url_);

    // HTTP GET the snapshot JPEG
    auto req = std::make_shared<HttpRequest>();
    req->method = HTTP_GET;
    req->url = snapshot_url_;
    req->timeout = HTTP_TIMEOUT_SEC;

    auto resp = requests::request(req);

    if (!running_.load())
        return false;

    if (!resp || resp->status_code < 200 || resp->status_code >= 300) {
        spdlog::debug("[SnapshotQR] Snapshot fetch failed (status={})",
                      resp ? static_cast<int>(resp->status_code) : -1);
        if (on_error_)
            on_error_("Snapshot fetch failed");
        return false;
    }

    if (resp->body.empty() || resp->body.size() > MAX_RESPONSE_BYTES) {
        spdlog::debug("[SnapshotQR] Invalid response size: {}", resp->body.size());
        return false;
    }

    // Decode JPEG with stb_image
    int width = 0, height = 0, channels = 0;
    uint8_t* pixels =
        stbi_load_from_memory(reinterpret_cast<const uint8_t*>(resp->body.data()),
                              static_cast<int>(resp->body.size()), &width, &height, &channels, 3);

    if (!pixels) {
        spdlog::debug("[SnapshotQR] JPEG decode failed: {}", stbi_failure_reason());
        return false;
    }

    if (width <= 0 || height <= 0 || width > MAX_IMAGE_DIMENSION || height > MAX_IMAGE_DIMENSION) {
        spdlog::warn("[SnapshotQR] Invalid image dimensions: {}x{}", width, height);
        stbi_image_free(pixels);
        return false;
    }

    // Ensure double buffers are allocated at correct size
    if (!decode_buf_ || frame_width_ != width || frame_height_ != height) {
        free_frame_bufs();
        decode_buf_ = create_draw_buf(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
        display_buf_ = create_draw_buf(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
        if (!decode_buf_ || !display_buf_) {
            spdlog::error("[SnapshotQR] Failed to allocate frame buffers {}x{}", width, height);
            stbi_image_free(pixels);
            return false;
        }
        frame_width_ = width;
        frame_height_ = height;
    }

    // Decode into decode_buf_ (poll thread owns this buffer, LVGL never reads it)
    auto* dst = static_cast<uint8_t*>(decode_buf_->data);
    int src_stride = width * 3;
    int dst_stride = static_cast<int>(decode_buf_->header.stride);

    for (int y = 0; y < height; y++) {
        const uint8_t* src_row = pixels + y * src_stride;
        uint8_t* dst_row = dst + y * dst_stride;
        for (int x = 0; x < width; x++) {
            dst_row[x * 3 + 0] = src_row[x * 3 + 2]; // B <- R
            dst_row[x * 3 + 1] = src_row[x * 3 + 1]; // G <- G
            dst_row[x * 3 + 2] = src_row[x * 3 + 0]; // R <- B
        }
    }

    stbi_image_free(pixels);

    // Subsample to grayscale for QR decode
    int max_dim = std::max(width, height);
    int step = std::max(1, max_dim / QR_MAX_DIMENSION);
    int qr_w = width / step;
    int qr_h = height / step;

    grayscale_buf_.resize(static_cast<size_t>(qr_w * qr_h));
    for (int y = 0; y < qr_h; y++) {
        const uint8_t* row = dst + (y * step) * dst_stride;
        for (int x = 0; x < qr_w; x++) {
            grayscale_buf_[static_cast<size_t>(y * qr_w + x)] =
                row[(x * step) * 3 + 1]; // Green channel as luma proxy
        }
    }

    // Swap buffers: decode_buf_ becomes display_buf_ (LVGL reads this),
    // old display_buf_ becomes decode_buf_ (poll thread writes next frame here)
    std::swap(decode_buf_, display_buf_);

    // Deliver the now-stable display buffer to UI
    frame_pending_ = true;
    if (on_frame_) {
        on_frame_(display_buf_);
    }

    // Run QR decode
    spdlog::info("[SnapshotQR] Running QR decode on {}x{} grayscale", qr_w, qr_h);
    auto result = qr_decoder_.decode(grayscale_buf_.data(), qr_w, qr_h);
    if (result.success && result.spool_id >= 0) {
        spdlog::info("[SnapshotQR] QR decoded: spool_id={}", result.spool_id);
        // Stop polling before firing callback — callback may trigger overlay close
        // which calls stop() → join() while we're still on the poll thread
        running_ = false;
        if (on_qr_result_) {
            on_qr_result_(result.spool_id);
        }
    }

    spdlog::trace("[SnapshotQR] Frame {}x{}, QR scan {}x{}", width, height, qr_w, qr_h);
    return true;
}

void SnapshotQrScanner::free_frame_bufs() {
    if (display_buf_) {
        destroy_draw_buf(display_buf_);
        display_buf_ = nullptr;
    }
    if (decode_buf_) {
        destroy_draw_buf(decode_buf_);
        decode_buf_ = nullptr;
    }
    frame_width_ = 0;
    frame_height_ = 0;
}

lv_draw_buf_t* SnapshotQrScanner::create_draw_buf(uint32_t w, uint32_t h) {
    auto* buf = static_cast<lv_draw_buf_t*>(calloc(1, sizeof(lv_draw_buf_t)));
    if (!buf)
        return nullptr;

    uint32_t stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_RGB888);
    uint32_t data_size = stride * h;

    auto* data = calloc(1, data_size);
    if (!data) {
        free(buf);
        return nullptr;
    }

    lv_draw_buf_init(buf, w, h, LV_COLOR_FORMAT_RGB888, stride, data, data_size);
    lv_draw_buf_set_flag(buf, LV_IMAGE_FLAGS_MODIFIABLE);
    return buf;
}

void SnapshotQrScanner::destroy_draw_buf(lv_draw_buf_t* buf) {
    if (buf) {
        free(buf->data);
        free(buf);
    }
}

} // namespace helix
