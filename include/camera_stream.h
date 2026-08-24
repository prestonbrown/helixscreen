// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lvgl.h"

// Camera features available on Pi/desktop. JPEG decoding tries libturbojpeg
// (SIMD-accelerated, loaded via dlopen) at runtime, falling back to stb_image.
#if HELIX_HAS_CAMERA

#include "async_lifetime_guard.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class HttpRequest;

namespace helix {

/**
 * @brief Camera image rotation (applied after decode, before LVGL delivery)
 */
enum class CameraRotation {
    None = 0,
    Rotate90 = 90,
    Rotate180 = 180,
    Rotate270 = 270,
};

/**
 * @brief MJPEG camera stream decoder with snapshot fallback
 *
 * Connects to an MJPEG stream URL, parses multipart boundaries, decodes
 * JPEG frames via libturbojpeg (SIMD-accelerated, runtime-loaded) or
 * stb_image (fallback), and delivers decoded RGB888 frames via callback.
 * Falls back to periodic snapshot polling if streaming fails.
 *
 * Threading: decode happens on a background thread. The on_frame callback is
 * called from that thread — callers must use ui_queue_update() to marshal to
 * the LVGL thread.
 */
class CameraStream {
  public:
    using FrameCallback = std::function<void(lv_draw_buf_t* frame)>;
    using ErrorCallback = std::function<void(const char* message)>;

    CameraStream();
    ~CameraStream();

    CameraStream(const CameraStream&) = delete;
    CameraStream& operator=(const CameraStream&) = delete;

    void set_flip(bool horizontal, bool vertical) {
        flip_h_.store(horizontal);
        flip_v_.store(vertical);
    }

    /**
     * @brief Set rotation applied after decode.
     *
     * 90/270 swap output dimensions; 180 is flip_h + flip_v.
     * Thread-safe — takes effect on next decoded frame.
     */
    void set_rotation(CameraRotation rotation) {
        rotation_.store(static_cast<int>(rotation));
    }

    CameraRotation get_rotation() const {
        return static_cast<CameraRotation>(rotation_.load());
    }

    /**
     * @brief Set target display size for decode-time downscaling.
     *
     * When set, turbojpeg decodes at the smallest scaling factor that produces
     * output >= target dimensions (accounting for rotation). This avoids
     * decoding full-resolution frames (e.g. 1920x1080) when the display
     * widget is much smaller (e.g. 800x480), eliminating expensive LVGL
     * software scaling during rendering.
     *
     * Pass (0, 0) to disable downscaling (decode at full camera resolution).
     * Thread-safe — takes effect on next decoded frame.
     */
    void set_target_size(int w, int h) {
        target_w_.store(w);
        target_h_.store(h);
    }

    /**
     * @brief Set maximum frame delivery rate.
     *
     * Frames arriving faster than this rate are discarded before decode.
     * 0 = paused (no frames delivered), -1 = unlimited.
     * Thread-safe — takes effect on next frame boundary.
     */
    void set_max_fps(int fps) {
        max_fps_.store(fps, std::memory_order_relaxed);
    }
    int get_max_fps() const {
        return max_fps_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Configure stream URLs from printer state.
     *
     * Reads webcam URLs from PrinterState and resolves relative URLs via
     * IMoonrakerAPI. Flip/rotation must be set separately by the caller.
     *
     * @param[out] stream_url Resolved stream URL (empty if no webcam)
     * @param[out] snapshot_url Resolved snapshot URL (empty if no webcam)
     * @return true if at least one URL is available
     */
    bool configure_from_printer(std::string& stream_url, std::string& snapshot_url);

    /**
     * @brief Copy pixels to LVGL BGR format with optional flip.
     *
     * When using stb_image (fallback), source is RGB and R↔B swap is needed.
     * When using turbojpeg, source is already BGR — only flip is applied.
     * Exposed as public static for unit testing.
     */
    static void copy_pixels_to_lvgl(const uint8_t* src, uint8_t* dst, int width, int height,
                                    int src_stride, int dst_stride, bool flip_h, bool flip_v,
                                    bool swap_rb);

    /**
     * @brief Transpose pixels (90° CW rotation) with optional R↔B swap.
     *
     * Input WxH → output HxW. Each pixel at (x,y) maps to (height-1-y, x).
     * Exposed as public static for unit testing.
     */
    static void transpose_pixels_cw(const uint8_t* src, uint8_t* dst, int src_width, int src_height,
                                    int src_stride, int dst_stride, bool swap_rb);

    // Legacy name kept for test compatibility
    static void copy_pixels_rgb_to_lvgl(const uint8_t* src, uint8_t* dst, int width, int height,
                                        int src_stride, int dst_stride, bool flip_h, bool flip_v) {
        copy_pixels_to_lvgl(src, dst, width, height, src_stride, dst_stride, flip_h, flip_v, true);
    }

    /**
     * @brief Parse a boundary string from a Content-Type header value.
     *
     * Extracts the boundary parameter, strips quotes, and prepends "--" if
     * needed. Returns empty string if no boundary found. Exposed for testing.
     */
    static std::string parse_boundary(const std::string& content_type);

    void start(const std::string& stream_url, const std::string& snapshot_url,
               FrameCallback on_frame, ErrorCallback on_error = nullptr);
    void stop();
    bool is_running() const;

    /// True if stop() timed out joining the stream thread and detached it.
    /// Callers must NOT destroy the CameraStream in this case — the detached
    /// thread still holds `this`.  Use unique_ptr::release() to intentionally
    /// leak and prevent use-after-free.
    bool was_detached() const {
        return thread_detached_;
    }

    /// Compute scaled decode dimensions for a given source size and target.
    /// Returns the source dimensions unchanged if no scaling is beneficial.
    /// Public for unit testing.
    struct ScaledSize {
        int w;
        int h;
    };
    ScaledSize compute_scaled_size(int src_w, int src_h) const;

  private:
    friend class CameraStreamTestAccess;

    int process_stream_data();
    void stream_thread_func();
    void snapshot_poll_loop();
    void fetch_snapshot();
    bool decode_jpeg(const uint8_t* data, size_t len);
    bool decode_jpeg_turbojpeg(const uint8_t* data, size_t len);
    bool decode_jpeg_stb(const uint8_t* data, size_t len);
    void deliver_frame();
    void ensure_buffers(int width, int height);
    void free_buffers();

    // Thread-safe error reporting: copies on_error_ under cb_mutex_, invokes outside lock.
    // Only invokes if the lifetime token is still valid.
    void report_error(const helix::LifetimeToken& token, const char* message);

    // Resolve rotation + flip atomics into output dimensions and effective transform
    struct TransformParams {
        CameraRotation rotation;
        bool flip_h;
        bool flip_v;
        bool needs_transpose;
        int out_w;
        int out_h;
    };
    TransformParams resolve_transform(int src_w, int src_h) const;

    // Apply transpose and/or flip from decoded pixels (src) into back_buf_
    void apply_pixel_transform(const uint8_t* src, int src_w, int src_h, int src_stride,
                               bool swap_rb, const TransformParams& params);

    std::string stream_url_;
    std::string snapshot_url_;
    FrameCallback on_frame_;
    ErrorCallback on_error_;
    std::atomic<bool> flip_h_{false};
    std::atomic<bool> flip_v_{false};
    std::atomic<int> rotation_{0}; // CameraRotation cast to int
    std::atomic<int> target_w_{0}; // Target display width (0 = no downscaling)
    std::atomic<int> target_h_{0}; // Target display height
    std::atomic<int> max_fps_{-1}; // -1 = unlimited (default until widget configures)
    std::chrono::steady_clock::time_point last_frame_time_; // Only accessed from stream thread

    // Draw buffer helpers — use system malloc (thread-safe) instead of
    // lv_draw_buf_create which calls lv_malloc (NOT thread-safe)
    static lv_draw_buf_t* create_draw_buf(uint32_t w, uint32_t h, lv_color_format_t cf);
    static void destroy_draw_buf(lv_draw_buf_t* buf);

    // Double buffer — decode into back, swap to front on delivery
    lv_draw_buf_t* front_buf_ = nullptr;
    lv_draw_buf_t* back_buf_ = nullptr;
    int frame_width_ = 0;
    int frame_height_ = 0;
    std::mutex buf_mutex_;
    std::mutex cb_mutex_; // Protects on_frame_ / on_error_ against TOCTOU races

    // Old front buffers awaiting safe cleanup — LVGL may still reference
    // them via lv_image_set_src until the widget clears the source
    std::vector<lv_draw_buf_t*> retired_bufs_;

    // Scratch buffers reused across frames to avoid per-frame heap allocation
    std::unique_ptr<uint8_t[]> transpose_buf_;
    size_t transpose_buf_size_ = 0;
    std::unique_ptr<uint8_t[]> decode_temp_; // temp for decode+transform path
    size_t decode_temp_size_ = 0;

    // MJPEG parser state
    std::vector<uint8_t> recv_buf_;
    std::string boundary_;

    // Active HTTP request — weak_ptr avoids refcount races between the stream
    // thread and libhv's internal thread pool.  stop() locks the weak_ptr
    // temporarily to call Cancel().
    std::weak_ptr<HttpRequest> active_req_;
    std::mutex req_mutex_;

    // State — lifetime_ tokens are captured by http_cb closures so they
    // can detect object destruction and bail out before touching members
    helix::AsyncLifetimeGuard lifetime_;
    std::atomic<bool> running_{false};
    std::atomic<bool> got_stream_data_{false}; // Set by http_cb when data arrives
    int stream_fail_count_ = 0;
    bool thread_detached_ = false; // Set by stop() if thread join times out
    std::thread stream_thread_;

    static constexpr int MAX_STREAM_FAILURES = 3;
    static constexpr int SNAPSHOT_INTERVAL_MS = 2000;
    static constexpr int STREAM_CONNECT_TIMEOUT_SEC = 5; // Initial connection attempt
    static constexpr int STREAM_TIMEOUT_SEC = 300;       // Active stream — reconnects on timeout

    // libturbojpeg runtime loading (dlopen) — nullptr if unavailable
    void* tj_lib_ = nullptr; // dlopen handle
    void* tj_ = nullptr;     // tjhandle (decompressor instance)
    // Function pointers loaded via dlsym
    using TjInitDecompress_t = void* (*)();
    using TjDecompressHeader3_t = int (*)(void*, const unsigned char*, unsigned long, int*, int*,
                                          int*, int*);
    using TjDecompress2_t = int (*)(void*, const unsigned char*, unsigned long, unsigned char*, int,
                                    int, int, int, int);
    using TjDestroy_t = int (*)(void*);
    using TjGetErrorStr2_t = char* (*)(void*);
    struct TjScalingFactor {
        int num;
        int denom;
    };
    using TjGetScalingFactors_t = TjScalingFactor* (*)(int*);
    TjDecompressHeader3_t fn_decompress_header_ = nullptr;
    TjDecompress2_t fn_decompress_ = nullptr;
    TjDestroy_t fn_destroy_ = nullptr;
    TjGetErrorStr2_t fn_get_error_ = nullptr;
    TjGetScalingFactors_t fn_get_scaling_factors_ = nullptr;
};

} // namespace helix

#endif // HELIX_HAS_CAMERA
