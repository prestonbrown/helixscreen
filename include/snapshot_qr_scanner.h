// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "async_lifetime_guard.h"
#include "lvgl.h"
#include "qr_decoder.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace helix {

/**
 * @brief Lightweight snapshot-polling QR scanner for platforms without full camera streaming.
 *
 * Polls a webcam snapshot URL via HTTP GET, decodes JPEG with stb_image,
 * produces a full-color lv_draw_buf_t for viewfinder display, and runs
 * QUIRC QR decode on a subsampled grayscale buffer.
 *
 * Memory: ~2.8MB steady state (single RGB888 frame buffer + grayscale QR buffer).
 * All buffers freed on stop().
 */
class SnapshotQrScanner {
  public:
    using FrameCallback = std::function<void(lv_draw_buf_t* frame)>;
    using QrResultCallback = std::function<void(int spool_id)>;
    using ErrorCallback = std::function<void(const char* message)>;

    SnapshotQrScanner();
    ~SnapshotQrScanner();

    SnapshotQrScanner(const SnapshotQrScanner&) = delete;
    SnapshotQrScanner& operator=(const SnapshotQrScanner&) = delete;

    /// Start polling the snapshot URL. Callbacks fire from background threads.
    void start(const std::string& snapshot_url, FrameCallback on_frame,
               QrResultCallback on_qr_result, ErrorCallback on_error = nullptr);

    /// Stop polling and free all buffers. Safe to call multiple times.
    void stop();

    bool is_running() const {
        return running_.load();
    }

    /// Signal that the UI has consumed the current frame (unblocks next fetch).
    void frame_consumed();

  private:
    void poll_loop();
    bool fetch_and_decode();
    void free_frame_bufs();

    static lv_draw_buf_t* create_draw_buf(uint32_t w, uint32_t h);
    static void destroy_draw_buf(lv_draw_buf_t* buf);

    std::string snapshot_url_;
    FrameCallback on_frame_;
    QrResultCallback on_qr_result_;
    ErrorCallback on_error_;

    std::thread poll_thread_;
    AsyncLifetimeGuard lifetime_;
    std::atomic<bool> running_{false};
    std::atomic<bool> frame_pending_{false};

    // Double-buffered: decode_buf_ is written by poll thread, display_buf_ is read by LVGL
    lv_draw_buf_t* display_buf_ = nullptr;
    lv_draw_buf_t* decode_buf_ = nullptr;
    int frame_width_ = 0;
    int frame_height_ = 0;

    QrDecoder qr_decoder_;
    std::vector<uint8_t> grayscale_buf_;

    static constexpr int POLL_INTERVAL_MS = 1000;
    static constexpr int POLL_STEP_MS = 100;
    static constexpr int MAX_BACKOFF_MS = 5000;
    static constexpr int HTTP_TIMEOUT_SEC = 10;
    static constexpr int QR_MAX_DIMENSION = 1280;
    static constexpr int MAX_IMAGE_DIMENSION = 4096;
    static constexpr size_t MAX_RESPONSE_BYTES = 8 * 1024 * 1024;
};

} // namespace helix
