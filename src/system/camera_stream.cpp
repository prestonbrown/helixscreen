// SPDX-License-Identifier: GPL-3.0-or-later

#include "camera_stream.h"

#include "lvgl.h"

#if HELIX_HAS_CAMERA

#include "app_globals.h"
#include "hv/requests.h"
#include "i_moonraker_api.h"
#include "printer_state.h"
#include "spdlog/spdlog.h"
#include "stb_image.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <dlfcn.h>
#include <string_view>

// TurboJPEG pixel format and flag constants (avoid header dependency)
static constexpr int TJ_PIXELFORMAT_BGR = 1;
static constexpr int TJ_FLAG_FASTDCT = 2048;

namespace helix {

namespace {

/// Runs a callable when the enclosing scope unwinds, on every path including
/// exceptions. Local to this file — nothing else in the tree needs one yet.
template <typename F> class ScopeExit {
  public:
    explicit ScopeExit(F fn) : fn_(std::move(fn)) {}
    ~ScopeExit() {
        fn_();
    }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

  private:
    F fn_;
};

} // namespace

// ============================================================================
// Construction / Destruction
// ============================================================================

CameraStream::CameraStream() {
    // Try to load libturbojpeg at runtime for SIMD-accelerated JPEG decode.
    // Falls back to stb_image (scalar) if the library isn't installed.
    // Versioned soname first — that is what every Linux target installs, so
    // nothing about their resolution changes. The unversioned name is the
    // Android case: the APK packager only accepts plain lib*.so, so the copy we
    // build into the APK is libturbojpeg.so (prestonbrown/helixscreen#1245).
    for (const char* soname : {"libturbojpeg.so.0", "libturbojpeg.so"}) {
        tj_lib_ = dlopen(soname, RTLD_LAZY);
        if (tj_lib_) {
            break;
        }
    }
    if (tj_lib_) {
        auto fn_init = reinterpret_cast<TjInitDecompress_t>(dlsym(tj_lib_, "tjInitDecompress"));
        fn_decompress_header_ =
            reinterpret_cast<TjDecompressHeader3_t>(dlsym(tj_lib_, "tjDecompressHeader3"));
        fn_decompress_ = reinterpret_cast<TjDecompress2_t>(dlsym(tj_lib_, "tjDecompress2"));
        fn_destroy_ = reinterpret_cast<TjDestroy_t>(dlsym(tj_lib_, "tjDestroy"));
        fn_get_error_ = reinterpret_cast<TjGetErrorStr2_t>(dlsym(tj_lib_, "tjGetErrorStr2"));

        fn_get_scaling_factors_ =
            reinterpret_cast<TjGetScalingFactors_t>(dlsym(tj_lib_, "tjGetScalingFactors"));

        if (fn_init && fn_decompress_header_ && fn_decompress_ && fn_destroy_ && fn_get_error_) {
            tj_ = fn_init();
            if (tj_) {
                spdlog::info("[CameraStream] Using libturbojpeg (SIMD-accelerated JPEG decode)");
            } else {
                spdlog::warn("[CameraStream] tjInitDecompress() failed, falling back to stb_image");
            }
        } else {
            spdlog::warn("[CameraStream] libturbojpeg loaded but missing symbols, "
                         "falling back to stb_image");
            dlclose(tj_lib_);
            tj_lib_ = nullptr;
        }
    } else {
        spdlog::info("[CameraStream] libturbojpeg not available, using stb_image for JPEG decode");
    }
}

CameraStream::~CameraStream() {
    stop();
    // Only clean up turbojpeg if stop() successfully joined the thread.
    // If the thread was detached (join timeout), it may still be calling
    // turbojpeg functions — dlclose would unmap the code and crash.
    if (!thread_detached_) {
        if (tj_ && fn_destroy_) {
            fn_destroy_(tj_);
            tj_ = nullptr;
        }
        if (tj_lib_) {
            dlclose(tj_lib_);
            tj_lib_ = nullptr;
        }
    }
}

// ============================================================================
// Public API
// ============================================================================

bool CameraStream::configure_from_printer(std::string& stream_url, std::string& snapshot_url) {
    // Lazy includes — avoid header dependency on printer_state/moonraker in camera_stream.h
    auto& state = get_printer_state();
    stream_url = state.get_webcam_stream_url();
    snapshot_url = state.get_webcam_snapshot_url();

    if (stream_url.empty() && snapshot_url.empty()) {
        return false;
    }

    // Resolve relative URLs against the web frontend (nginx on port 80)
    auto* api = get_moonraker_api();
    if (api) {
        api->resolve_webcam_url(stream_url);
        api->resolve_webcam_url(snapshot_url);
    }

    // Flip/rotation are set by the caller (CameraWidget::apply_transform)
    // which XORs Moonraker values with user overrides.

    return true;
}

void CameraStream::start(const std::string& stream_url, const std::string& snapshot_url,
                         FrameCallback on_frame, ErrorCallback on_error) {
    // A worker that exited on its own leaves running_ false but the std::thread
    // object still joinable — and assigning a fresh thread over a joinable one
    // is std::terminate. Reap it before rebuilding any state.
    if (running_.load() || stream_thread_.joinable()) {
        stop();
    }

    stream_url_ = stream_url;
    snapshot_url_ = snapshot_url;
    on_frame_ = std::move(on_frame);
    on_error_ = std::move(on_error);
    stream_fail_count_ = 0;
    running_.store(true);

    spdlog::info("[CameraStream] Starting — stream={}, snapshot={}", stream_url_, snapshot_url_);

    if (!stream_url_.empty()) {
        spdlog::info("[CameraStream] Using MJPEG streaming mode");
        stream_thread_ = std::thread(&CameraStream::stream_thread_func, this);
    } else if (!snapshot_url_.empty()) {
        spdlog::info("[CameraStream] Using snapshot mode (interval={}ms)", SNAPSHOT_INTERVAL_MS);
        stream_thread_ = std::thread(&CameraStream::snapshot_poll_loop, this);
    } else {
        spdlog::warn("[CameraStream] No stream or snapshot URL provided");
        running_.store(false);
    }
}

void CameraStream::stop() {
    // Invalidate lifetime guard FIRST — http_cb closures capture a token
    // and bail out before accessing any member state
    lifetime_.invalidate();

    bool was_running = running_.exchange(false);

    if (was_running) {
        spdlog::info("[CameraStream] Stopping");
    }

    // Cancel any in-flight HTTP request to unblock the stream thread
    {
        std::lock_guard<std::mutex> lock(req_mutex_);
        if (auto req = active_req_.lock()) {
            req->Cancel();
        }
    }

    // Snapshot before the join below clears joinability. The worker clears
    // running_ itself on every exit path, so `was_running` alone no longer
    // proves there is state to reclaim — a worker that gave up on its own
    // still leaves draw buffers and callbacks behind.
    bool had_thread = stream_thread_.joinable();

    // Join the stream thread with periodic re-cancellation. Use a helper
    // thread for timed join — destroying a joinable std::thread is fatal.
    bool thread_joined = true;
    if (stream_thread_.joinable()) {
        auto joined = std::make_shared<std::atomic<bool>>(false);
        // Under thread exhaustion this constructor can throw
        // std::system_error(EAGAIN). Destructor paths must not let it
        // escape — detach the stream thread and skip the timed join.
        std::thread join_helper;
        bool helper_spawned = false;
        try {
            join_helper = std::thread([this, joined]() {
                stream_thread_.join();
                joined->store(true);
            });
            helper_spawned = true;
        } catch (const std::system_error& e) {
            spdlog::warn("[CameraStream] Failed to spawn join_helper ({}) — "
                         "detaching stream thread",
                         e.what());
            stream_thread_.detach();
            thread_joined = false;
            thread_detached_ = true;
        }
        if (helper_spawned) {
            constexpr auto JOIN_TIMEOUT = std::chrono::seconds(5);
            constexpr auto CANCEL_INTERVAL = std::chrono::milliseconds(200);
            auto deadline = std::chrono::steady_clock::now() + JOIN_TIMEOUT;

            while (!joined->load()) {
                if (std::chrono::steady_clock::now() > deadline) {
                    spdlog::error("[CameraStream] Thread join timed out after 5s, "
                                  "detaching (lifetime_ guard prevents UAF)");
                    join_helper.detach();
                    stream_thread_.detach();
                    thread_joined = false;
                    thread_detached_ = true;
                    break;
                }

                // Re-cancel — thread may have started a new HTTP request
                {
                    std::lock_guard<std::mutex> lock(req_mutex_);
                    if (auto req = active_req_.lock()) {
                        req->Cancel();
                    }
                }
                std::this_thread::sleep_for(CANCEL_INTERVAL);
            }

            if (join_helper.joinable()) {
                join_helper.join();
            }
        } // end if (helper_spawned)
    }

    if (was_running || had_thread) {
        if (thread_joined) {
            // Thread exited — safe to free everything
            free_buffers();
            recv_buf_.clear();
            boundary_.clear();
            {
                std::lock_guard<std::mutex> lock(cb_mutex_);
                on_frame_ = nullptr;
                on_error_ = nullptr;
            }
        } else {
            // Thread still running with lifetime_ invalidated — don't touch state it
            // may reference. Leak buffers and callbacks to avoid UAF.
            spdlog::warn("[CameraStream] Leaking state (stream thread still running)");
            std::lock_guard<std::mutex> lock(buf_mutex_);
            front_buf_ = nullptr;
            back_buf_ = nullptr;
            retired_bufs_.clear();
            frame_width_ = 0;
            frame_height_ = 0;
        }
    }
}

bool CameraStream::is_running() const {
    return running_.load();
}

// ============================================================================
// Boundary Parsing
// ============================================================================

std::string CameraStream::parse_boundary(const std::string& content_type) {
    auto pos = content_type.find("boundary=");
    if (pos == std::string::npos) {
        return {};
    }

    std::string boundary = content_type.substr(pos + 9);
    // Strip trailing parameters (after ';' or whitespace)
    auto end = boundary.find_first_of("; \t");
    if (end != std::string::npos) {
        boundary = boundary.substr(0, end);
    }
    // Strip quotes
    if (!boundary.empty() && boundary.front() == '"') {
        boundary = boundary.substr(1);
    }
    if (!boundary.empty() && boundary.back() == '"') {
        boundary.pop_back();
    }
    // Ensure "--" prefix
    if (boundary.size() >= 2 && boundary.substr(0, 2) == "--") {
        return boundary;
    }
    return "--" + boundary;
}

// ============================================================================
// MJPEG Stream Thread (http_cb streaming)
// ============================================================================

void CameraStream::stream_thread_func() {
    // Capture a lifetime token so exception handlers can check
    // object validity even after CameraStream may be destroyed
    auto thread_token = lifetime_.token();

    // running_ means "the worker is alive", so it has to be cleared wherever
    // the worker actually stops — not only in stop(). The body below exits on
    // its own in several ways (failure budget exhausted with no snapshot URL
    // to fall back to, std::bad_alloc, any other exception), and leaving the
    // flag set on those paths makes the camera permanently unrevivable:
    // CameraWidget::start_stream() skips the restart while is_running() is
    // true, so the feed stays dead until the user switches tabs (#1245).
    // Scope-level so no future early return can regress it; it fires after
    // the snapshot fallback below, which needs the flag to stay set while it
    // is legitimately polling.
    ScopeExit clear_running([this] { running_.store(false); });

    try {
        spdlog::debug("[CameraStream] Stream thread started for {}", stream_url_);
        recv_buf_.clear();
        recv_buf_.reserve(256 * 1024);
        boundary_.clear();
        got_stream_data_.store(false);
        bool ever_connected = false;

        while (running_.load() && stream_fail_count_ < MAX_STREAM_FAILURES) {
            auto req = std::make_shared<HttpRequest>();
            req->method = HTTP_GET;
            req->url = stream_url_;
            // Short timeout until the first successful connection, then long
            // timeout for the persistent stream. MJPEG responses are infinite —
            // the timeout just drives periodic reconnection.
            int timeout = ever_connected ? STREAM_TIMEOUT_SEC : STREAM_CONNECT_TIMEOUT_SEC;
            req->timeout = timeout;
            spdlog::debug(
                "[CameraStream] Attempting stream connection to {} (timeout={}s, attempt={})",
                stream_url_, timeout, stream_fail_count_ + 1);

            // Store weak reference for cancellation by stop() — weak_ptr avoids
            // the shared_ptr refcount race that caused SIGSEGV in _M_release()
            {
                std::lock_guard<std::mutex> lock(req_mutex_);
                active_req_ = req; // weak_ptr assignment from shared_ptr
            }

            // Set up http_cb for incremental MJPEG parsing. The callback fires as
            // HTTP data arrives: HP_HEADERS_COMPLETE once headers are ready, then
            // HP_BODY repeatedly with each chunk of the multipart body.
            // Capture lifetime token — safe to check even after object destruction
            auto cb_token = lifetime_.token();
            req->http_cb = [this, cb_token](HttpMessage* resp, http_parser_state state,
                                            const char* data, size_t size) {
                // Check lifetime first — object may be destroyed or shutting down.
                // L081_OK: dtor joins libhv handler; buf+boundary exclusive to stream-thread.
                if (cb_token.expired_no_lvgl())
                    return; // L081_OK

                if (!running_.load()) {
                    // Cancel the request from within the callback
                    std::lock_guard<std::mutex> lock(req_mutex_);
                    if (auto r = active_req_.lock()) {
                        r->Cancel();
                    }
                    return;
                }

                if (state == HP_HEADERS_COMPLETE) {
                    std::string content_type = resp->GetHeader("Content-Type");
                    boundary_ = parse_boundary(content_type);
                    if (boundary_.empty()) {
                        spdlog::warn("[CameraStream] No boundary in Content-Type: {}",
                                     content_type);
                    } else {
                        got_stream_data_.store(true);
                    }
                    spdlog::debug("[CameraStream] Stream connected, boundary='{}'", boundary_);
                } else if (state == HP_BODY) {
                    if (data && size > 0) {
                        recv_buf_.insert(recv_buf_.end(), data, data + size);

                        // Cap buffer growth — if camera sends faster than we decode,
                        // drop stale data to prevent unbounded memory growth
                        if (recv_buf_.size() > 4 * 1024 * 1024) {
                            spdlog::warn("[CameraStream] recv_buf_ exceeded 4MB, clearing");
                            recv_buf_.clear();
                        } else if (!boundary_.empty()) {
                            process_stream_data();
                        }
                    }
                }
            };

            auto resp = requests::request(req);
            // No need to clear active_req_ — it's a weak_ptr that expires
            // naturally when the local `req` goes out of scope at loop end.

            if (!running_.load()) {
                break;
            }

            // If boundary was empty and we got body data, try as single JPEG
            if (boundary_.empty() && resp && !resp->body.empty()) {
                auto* body_data = reinterpret_cast<const uint8_t*>(resp->body.data());
                if (decode_jpeg(body_data, resp->body.size())) {
                    deliver_frame();
                }
            }

            // Evaluate success: MJPEG streams are infinite so requests::request()
            // always returns with status=-1 (timeout). If http_cb received data,
            // the stream was healthy — the timeout just ended a streaming session.
            bool had_data = got_stream_data_.load();
            bool got_response = resp && resp->status_code >= 200 && resp->status_code < 300;

            if (had_data) {
                // Stream was flowing — timeout is normal, not a failure
                spdlog::debug("[CameraStream] Stream session ended (received data, reconnecting)");
                stream_fail_count_ = 0;
                ever_connected = true;
            } else if (!got_response) {
                spdlog::warn("[CameraStream] Stream request failed (status={})",
                             resp ? static_cast<int>(resp->status_code) : -1);
                stream_fail_count_++;
            } else if (boundary_.empty()) {
                // Got HTTP 200 but no multipart boundary — not an MJPEG stream
                stream_fail_count_++;
            } else {
                stream_fail_count_ = 0;
            }

            // Clear partial data before reconnecting
            recv_buf_.clear();
            boundary_.clear();
            got_stream_data_.store(false);

            if (running_.load() && stream_fail_count_ < MAX_STREAM_FAILURES) {
                // Brief backoff before reconnecting
                int backoff_ms = std::min(1000 * stream_fail_count_, 5000);
                if (backoff_ms > 0) {
                    for (int i = 0; i < backoff_ms / 100 && running_.load(); i++) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                }
            }
        }

        // Fall back to snapshot mode if streaming failed
        if (running_.load() && stream_fail_count_ >= MAX_STREAM_FAILURES) {
            spdlog::warn("[CameraStream] Stream failed {} times, falling back to snapshot mode",
                         stream_fail_count_);
            if (!snapshot_url_.empty()) {
                report_error(thread_token, "Stream failed, trying snapshots...");
                snapshot_poll_loop();
            } else {
                report_error(thread_token, "Stream unavailable");
            }
        }

        spdlog::debug("[CameraStream] Stream thread exiting");
    } catch (const std::bad_alloc& e) {
        spdlog::error("[CameraStream] Out of memory in stream thread: {}", e.what());
        if (!thread_token.expired_no_lvgl()) { // L081_OK: stream_thread_ dtor-joined; recv_buf_
                                               // thread-exclusive
            recv_buf_.clear();
            recv_buf_.shrink_to_fit();
            report_error(thread_token, "Out of memory");
        }
    } catch (const std::exception& e) {
        spdlog::error("[CameraStream] Uncaught exception in stream thread: {}", e.what());
        report_error(thread_token, e.what());
    }
}

// ============================================================================
// Incremental MJPEG Parser
// ============================================================================

int CameraStream::process_stream_data() {
    int frames_decoded = 0;

    while (running_.load()) {
        // Find boundary in the receive buffer (string_view avoids copying)
        std::string_view buf_view(reinterpret_cast<const char*>(recv_buf_.data()),
                                  recv_buf_.size());
        auto bpos = buf_view.find(boundary_);
        if (bpos == std::string::npos) {
            break;
        }

        // Find end of part headers (double CRLF after boundary)
        auto header_end = buf_view.find("\r\n\r\n", bpos);
        if (header_end == std::string::npos) {
            // Incomplete headers — wait for more data
            break;
        }
        size_t jpeg_start = header_end + 4;

        // Find next boundary to delimit JPEG data end
        auto next_bpos = buf_view.find(boundary_, jpeg_start);
        if (next_bpos == std::string::npos) {
            // No closing boundary yet — wait for more data (unless buffer is huge,
            // in which case the data before the current boundary is stale)
            if (bpos > 0) {
                // Trim everything before the current boundary to limit growth
                recv_buf_.erase(recv_buf_.begin(), recv_buf_.begin() + static_cast<long>(bpos));
            }
            break;
        }

        // Extract JPEG data, strip trailing CRLF
        size_t jpeg_len = next_bpos - jpeg_start;
        while (jpeg_len > 0 && (recv_buf_[jpeg_start + jpeg_len - 1] == '\r' ||
                                recv_buf_[jpeg_start + jpeg_len - 1] == '\n')) {
            jpeg_len--;
        }

        // FPS gate — skip decode entirely when paused or rate-limited.
        // Checked here (before decode_jpeg) to avoid wasting CPU on frames
        // that will be discarded anyway.
        bool skip_frame = false;
        {
            int fps = max_fps_.load(std::memory_order_relaxed);
            if (fps == 0) {
                // Paused — discard frame, consume data to keep parser in sync
                skip_frame = true;
            } else if (fps > 0) {
                auto now = std::chrono::steady_clock::now();
                auto min_interval = std::chrono::microseconds(1000000 / fps);
                if (now - last_frame_time_ < min_interval) {
                    skip_frame = true;
                }
            }
        }

        bool decoded = false;
        if (jpeg_len > 0 && !skip_frame) {
            decoded = decode_jpeg(recv_buf_.data() + jpeg_start, jpeg_len);
            if (decoded) {
                deliver_frame();
                frames_decoded++;
            }
        }

        // Erase consumed data up to the next boundary
        recv_buf_.erase(recv_buf_.begin(), recv_buf_.begin() + static_cast<long>(next_bpos));
    }

    return frames_decoded;
}

void CameraStream::snapshot_poll_loop() {
    // Capture lifetime token — if the thread is detached and the CameraStream
    // destroyed, the token detects invalidation for safe checking.
    auto poll_token = lifetime_.token();

    spdlog::info("[CameraStream] Starting snapshot poll loop (interval={}ms)",
                 SNAPSHOT_INTERVAL_MS);

    // L081_OK: loop conditions on the thread the owner joins; no LVGL below.
    while (!poll_token.expired_no_lvgl() && running_.load()) {
        fetch_snapshot();
        // Sleep in small increments to check running_ flag
        for (int i = 0;
             i < SNAPSHOT_INTERVAL_MS / 100 && !poll_token.expired_no_lvgl() && running_.load();
             i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void CameraStream::fetch_snapshot() {
    // Capture lifetime token — if the thread is detached and the CameraStream
    // destroyed during the blocking HTTP call, the token detects invalidation
    // so we can check before accessing member state (UAF prevention).
    auto snap_token = lifetime_.token();

    if (snapshot_url_.empty()) {
        return;
    }

    // FPS gate — skip fetch entirely when paused or rate-limited
    {
        int fps = max_fps_.load(std::memory_order_relaxed);
        if (fps == 0) {
            return; // Paused — skip snapshot fetch
        }
        if (fps > 0) {
            auto now = std::chrono::steady_clock::now();
            auto min_interval = std::chrono::microseconds(1000000 / fps);
            if (now - last_frame_time_ < min_interval) {
                return; // Too soon — skip this snapshot
            }
        }
    }

    spdlog::trace("[CameraStream] Fetching snapshot from {}", snapshot_url_);

    auto req = std::make_shared<HttpRequest>();
    req->method = HTTP_GET;
    req->url = snapshot_url_;
    req->timeout = STREAM_TIMEOUT_SEC;

    // Store weak reference for cancellation by stop()
    {
        std::lock_guard<std::mutex> lock(req_mutex_);
        active_req_ = req; // weak_ptr assignment from shared_ptr
    }

    auto resp = requests::request(req);

    // After the blocking HTTP call, stop() may have run and the CameraStream
    // may be destroyed (detached thread). Check lifetime BEFORE touching members.
    if (snap_token.expired_no_lvgl()) { // L081_OK: guards members, not LVGL
        return;
    }

    // Check running_ after blocking HTTP call — stop() may have been called
    if (!running_.load()) {
        return;
    }

    if (!resp || resp->status_code < 200 || resp->status_code >= 300) {
        spdlog::debug("[CameraStream] Snapshot fetch failed (status={})",
                      resp ? static_cast<int>(resp->status_code) : -1);
        return;
    }

    if (resp->body.empty()) {
        return;
    }

    auto* data = reinterpret_cast<const uint8_t*>(resp->body.data());
    if (decode_jpeg(data, resp->body.size())) {
        deliver_frame();
    }
}

// ============================================================================
// Pixel Copy with optional R↔B swap and flip
// ============================================================================

void CameraStream::copy_pixels_to_lvgl(const uint8_t* src, uint8_t* dst, int width, int height,
                                       int src_stride, int dst_stride, bool flip_h, bool flip_v,
                                       bool swap_rb) {
    for (int y = 0; y < height; y++) {
        int src_y = flip_v ? (height - 1 - y) : y;
        const uint8_t* src_row = src + src_y * src_stride;
        uint8_t* dst_row = dst + y * dst_stride;

        if (!flip_h && !swap_rb) {
            // Fast path: straight memcpy (turbojpeg BGR, no flip)
            std::memcpy(dst_row, src_row, static_cast<size_t>(width) * 3);
        } else if (flip_h && swap_rb) {
            for (int x = 0; x < width; x++) {
                int src_x = width - 1 - x;
                dst_row[x * 3 + 0] = src_row[src_x * 3 + 2];
                dst_row[x * 3 + 1] = src_row[src_x * 3 + 1];
                dst_row[x * 3 + 2] = src_row[src_x * 3 + 0];
            }
        } else if (flip_h) {
            for (int x = 0; x < width; x++) {
                int src_x = width - 1 - x;
                dst_row[x * 3 + 0] = src_row[src_x * 3 + 0];
                dst_row[x * 3 + 1] = src_row[src_x * 3 + 1];
                dst_row[x * 3 + 2] = src_row[src_x * 3 + 2];
            }
        } else {
            // swap_rb only, no flip
            for (int x = 0; x < width; x++) {
                dst_row[x * 3 + 0] = src_row[x * 3 + 2];
                dst_row[x * 3 + 1] = src_row[x * 3 + 1];
                dst_row[x * 3 + 2] = src_row[x * 3 + 0];
            }
        }
    }
}

// ============================================================================
// Pixel Transpose (90° CW rotation)
// ============================================================================

void CameraStream::transpose_pixels_cw(const uint8_t* src, uint8_t* dst, int src_width,
                                       int src_height, int src_stride, int dst_stride,
                                       bool swap_rb) {
    // 90° CW: pixel at (x, y) → (src_height - 1 - y, x)
    // Output dimensions: width = src_height, height = src_width
    for (int y = 0; y < src_height; y++) {
        const uint8_t* src_row = src + y * src_stride;
        int dst_x = src_height - 1 - y;
        for (int x = 0; x < src_width; x++) {
            uint8_t* dst_pixel = dst + x * dst_stride + dst_x * 3;
            if (swap_rb) {
                dst_pixel[0] = src_row[x * 3 + 2];
                dst_pixel[1] = src_row[x * 3 + 1];
                dst_pixel[2] = src_row[x * 3 + 0];
            } else {
                dst_pixel[0] = src_row[x * 3 + 0];
                dst_pixel[1] = src_row[x * 3 + 1];
                dst_pixel[2] = src_row[x * 3 + 2];
            }
        }
    }
}

// ============================================================================
// Transform Helpers
// ============================================================================

CameraStream::TransformParams CameraStream::resolve_transform(int src_w, int src_h) const {
    TransformParams p;
    p.rotation = static_cast<CameraRotation>(rotation_.load());
    p.flip_h = flip_h_.load();
    p.flip_v = flip_v_.load();

    // 180° is equivalent to flip_h + flip_v
    if (p.rotation == CameraRotation::Rotate180) {
        p.flip_h = !p.flip_h;
        p.flip_v = !p.flip_v;
        p.rotation = CameraRotation::None;
    }

    p.needs_transpose =
        (p.rotation == CameraRotation::Rotate90 || p.rotation == CameraRotation::Rotate270);
    p.out_w = p.needs_transpose ? src_h : src_w;
    p.out_h = p.needs_transpose ? src_w : src_h;
    return p;
}

void CameraStream::apply_pixel_transform(const uint8_t* src, int src_w, int src_h, int src_stride,
                                         bool swap_rb, const TransformParams& params) {
    auto* dst = static_cast<uint8_t*>(back_buf_->data);
    int dst_stride = static_cast<int>(back_buf_->header.stride);

    if (params.needs_transpose) {
        // 270° CW = 90° CW + flip both axes
        bool eff_fh = params.flip_h;
        bool eff_fv = params.flip_v;
        if (params.rotation == CameraRotation::Rotate270) {
            eff_fh = !eff_fh;
            eff_fv = !eff_fv;
        }

        if (!eff_fh && !eff_fv) {
            transpose_pixels_cw(src, dst, src_w, src_h, src_stride, dst_stride, swap_rb);
        } else {
            // Transpose to cached scratch buffer, then flip-copy to dst
            int trans_stride = params.out_w * 3;
            auto trans_size = static_cast<size_t>(trans_stride) * static_cast<size_t>(params.out_h);
            if (trans_size > transpose_buf_size_) {
                transpose_buf_ = std::make_unique<uint8_t[]>(trans_size);
                transpose_buf_size_ = trans_size;
            }
            transpose_pixels_cw(src, transpose_buf_.get(), src_w, src_h, src_stride, trans_stride,
                                swap_rb);
            copy_pixels_to_lvgl(transpose_buf_.get(), dst, params.out_w, params.out_h, trans_stride,
                                dst_stride, eff_fh, eff_fv, false);
        }
    } else {
        copy_pixels_to_lvgl(src, dst, src_w, src_h, src_stride, dst_stride, params.flip_h,
                            params.flip_v, swap_rb);
    }
}

// ============================================================================
// Decode-time Downscaling
// ============================================================================

CameraStream::ScaledSize CameraStream::compute_scaled_size(int src_w, int src_h) const {
    int tw = target_w_.load();
    int th = target_h_.load();

    if (tw <= 0 || th <= 0 || !fn_get_scaling_factors_) {
        return {src_w, src_h};
    }

    // Account for rotation: if 90/270, the output is transposed,
    // so we need camera-space targets swapped
    auto params = resolve_transform(src_w, src_h);
    if (params.needs_transpose) {
        std::swap(tw, th);
    }

    // Already at or below target — no scaling needed
    if (src_w <= tw && src_h <= th) {
        return {src_w, src_h};
    }

    int num_factors = 0;
    auto* factors = fn_get_scaling_factors_(&num_factors);
    if (!factors || num_factors <= 0) {
        return {src_w, src_h};
    }

    // turbojpeg scaling factors are sorted largest-first (2/1, 15/8, ..., 1/8).
    // Find the smallest factor where both scaled dimensions >= target.
    int best_w = src_w;
    int best_h = src_h;
    for (int i = 0; i < num_factors; i++) {
        int sw = (src_w * factors[i].num + factors[i].denom - 1) / factors[i].denom;
        int sh = (src_h * factors[i].num + factors[i].denom - 1) / factors[i].denom;
        if (sw >= tw && sh >= th) {
            best_w = sw;
            best_h = sh;
        } else {
            break; // Factors get smaller, so all subsequent will also be too small
        }
    }

    return {best_w, best_h};
}

// ============================================================================
// JPEG Decode
// ============================================================================

bool CameraStream::decode_jpeg(const uint8_t* data, size_t len) {
    if (!data || len == 0) {
        return false;
    }

    // Validate JPEG SOI marker and minimum viable size.
    // A valid JPEG needs SOI (2) + at least one marker segment (4) + EOI (2) = 8 bytes minimum.
    // In practice, camera frames should be much larger — reject suspiciously tiny payloads
    // that can cause NULL dereferences inside turbojpeg (see issue #552).
    constexpr size_t MIN_JPEG_SIZE = 64;
    if (len < MIN_JPEG_SIZE || data[0] != 0xFF || data[1] != 0xD8) {
        spdlog::debug("[CameraStream] Invalid JPEG data (len={}, need >= {})", len, MIN_JPEG_SIZE);
        return false;
    }
    // After SOI, the next byte must be 0xFF (start of a marker segment).
    // Corrupt/truncated frames that pass SOI but have garbage after can crash turbojpeg.
    if (data[2] != 0xFF) {
        spdlog::debug("[CameraStream] Malformed JPEG: no marker after SOI (0x{:02X})", data[2]);
        return false;
    }

    // Use turbojpeg if available, otherwise stb_image
    if (tj_) {
        return decode_jpeg_turbojpeg(data, len);
    }
    return decode_jpeg_stb(data, len);
}

bool CameraStream::decode_jpeg_turbojpeg(const uint8_t* data, size_t len) {
    int width = 0;
    int height = 0;
    int subsamp = 0;
    int colorspace = 0;

    if (fn_decompress_header_(tj_, data, static_cast<unsigned long>(len), &width, &height, &subsamp,
                              &colorspace) != 0) {
        spdlog::debug("[CameraStream] JPEG header parse failed: {}", fn_get_error_(tj_));
        return false;
    }

    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
        spdlog::warn("[CameraStream] Invalid JPEG dimensions: {}x{}", width, height);
        return false;
    }

    // Decode-time downscaling: decode at the smallest turbojpeg scaling factor
    // that still meets the target display size. For a 1920x1080 camera on an
    // 800x480 display, this typically picks 1/2 scale (960x540), avoiding
    // expensive LVGL software scaling during rendering.
    auto scaled = compute_scaled_size(width, height);
    int decode_w = scaled.w;
    int decode_h = scaled.h;

    auto params = resolve_transform(decode_w, decode_h);
    ensure_buffers(params.out_w, params.out_h);

    if (!back_buf_) {
        return false;
    }

    bool need_transform = params.flip_h || params.flip_v || params.needs_transpose;

    if (!need_transform) {
        // Fast path: decode directly into LVGL buffer as BGR
        auto* dst = static_cast<uint8_t*>(back_buf_->data);
        int dst_stride = static_cast<int>(back_buf_->header.stride);
        if (fn_decompress_(tj_, data, static_cast<unsigned long>(len), dst, decode_w, dst_stride,
                           decode_h, TJ_PIXELFORMAT_BGR, TJ_FLAG_FASTDCT) != 0) {
            spdlog::debug("[CameraStream] JPEG decode failed: {}", fn_get_error_(tj_));
            return false;
        }
    } else {
        // Decode to reusable temp buffer, then transform
        int src_stride = decode_w * 3;
        auto temp_size = static_cast<size_t>(src_stride) * static_cast<size_t>(decode_h);
        if (temp_size > decode_temp_size_) {
            decode_temp_ = std::make_unique<uint8_t[]>(temp_size);
            decode_temp_size_ = temp_size;
        }

        if (fn_decompress_(tj_, data, static_cast<unsigned long>(len), decode_temp_.get(), decode_w,
                           src_stride, decode_h, TJ_PIXELFORMAT_BGR, TJ_FLAG_FASTDCT) != 0) {
            spdlog::debug("[CameraStream] JPEG decode failed: {}", fn_get_error_(tj_));
            return false;
        }

        apply_pixel_transform(decode_temp_.get(), decode_w, decode_h, src_stride, false, params);
    }

    if (decode_w != width || decode_h != height) {
        spdlog::trace("[CameraStream] Decoded frame {}x{} → {}x{} → {}x{} (turbojpeg, downscaled)",
                      width, height, decode_w, decode_h, params.out_w, params.out_h);
    } else {
        spdlog::trace("[CameraStream] Decoded frame {}x{} → {}x{} (turbojpeg)", width, height,
                      params.out_w, params.out_h);
    }
    return true;
}

bool CameraStream::decode_jpeg_stb(const uint8_t* data, size_t len) {
    int width = 0;
    int height = 0;
    int channels = 0;

    uint8_t* pixels =
        stbi_load_from_memory(data, static_cast<int>(len), &width, &height, &channels, 3);
    if (!pixels) {
        spdlog::debug("[CameraStream] JPEG decode failed: {}", stbi_failure_reason());
        return false;
    }

    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
        spdlog::warn("[CameraStream] Invalid JPEG dimensions: {}x{}", width, height);
        stbi_image_free(pixels);
        return false;
    }

    auto params = resolve_transform(width, height);
    ensure_buffers(params.out_w, params.out_h);

    if (!back_buf_) {
        stbi_image_free(pixels);
        return false;
    }

    // stb_image outputs RGB, LVGL stores BGR — need R↔B swap
    int src_stride = width * 3;
    apply_pixel_transform(pixels, width, height, src_stride, true, params);

    stbi_image_free(pixels);
    spdlog::trace("[CameraStream] Decoded frame {}x{} → {}x{} (stb_image)", width, height,
                  params.out_w, params.out_h);
    return true;
}

// ============================================================================
// Error Reporting
// ============================================================================

void CameraStream::report_error(const helix::LifetimeToken& token, const char* message) {
    // Defer cb_mutex_ + on_error_ access to main thread to satisfy the L081
    // detector and to guarantee `this` validity (the worker thread is
    // dtor-joined, but the lifetime instrumentation flags any bg-thread
    // member access reached via tok.expired()).
    std::string msg = message ? message : "";
    token.defer("CameraStream::report_error", [this, msg = std::move(msg)]() mutable {
        ErrorCallback err_cb;
        {
            std::lock_guard<std::mutex> lock(cb_mutex_);
            err_cb = on_error_;
        }
        if (err_cb)
            err_cb(msg.c_str());
    });
}

// ============================================================================
// Frame Delivery
// ============================================================================

void CameraStream::deliver_frame() {
    if (!running_.load()) {
        return;
    }

    // Update frame timestamp for fps gate
    last_frame_time_ = std::chrono::steady_clock::now();

    FrameCallback frame_cb;
    {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        if (!on_frame_)
            return;
        frame_cb = on_frame_;
    }

    if (!back_buf_)
        return;

    {
        std::lock_guard<std::mutex> lock(buf_mutex_);
        std::swap(front_buf_, back_buf_);
    }

    // No frame_pending_ gate — camera continues decoding immediately.
    // The callback queues lv_image_set_src() to the UI thread, which
    // processes it BEFORE rendering (UpdateQueue design). Typically the
    // ~3ms decode time (with downscaling) exceeds the UI tick interval,
    // so set_src runs before the next swap. If the UI thread stalls and
    // two swaps occur between ticks, the camera may write to a buffer
    // LVGL is still rendering — this can cause a torn frame (visual
    // artifact) but NOT a crash, since both buffers remain allocated.
    // The throughput gain (~3x fps) outweighs occasional tearing.
    frame_cb(front_buf_);
}

// ============================================================================
// Buffer Management
// ============================================================================

// Allocate a draw buffer using the system allocator (calloc/free) instead of
// lv_draw_buf_create which calls lv_malloc — LVGL's allocator is NOT
// thread-safe and these run on the background stream thread.
lv_draw_buf_t* CameraStream::create_draw_buf(uint32_t w, uint32_t h, lv_color_format_t cf) {
    auto* buf = static_cast<lv_draw_buf_t*>(calloc(1, sizeof(lv_draw_buf_t)));
    if (!buf)
        return nullptr;

    uint32_t stride = lv_draw_buf_width_to_stride(w, cf);
    uint32_t data_size = stride * h;

    auto* data = calloc(1, data_size);
    if (!data) {
        free(buf);
        return nullptr;
    }

    lv_draw_buf_init(buf, w, h, cf, stride, data, data_size);
    lv_draw_buf_set_flag(buf, LV_IMAGE_FLAGS_MODIFIABLE);
    return buf;
}

void CameraStream::destroy_draw_buf(lv_draw_buf_t* buf) {
    if (!buf)
        return;
    free(buf->data);
    free(buf);
}

void CameraStream::ensure_buffers(int width, int height) {
    std::lock_guard<std::mutex> lock(buf_mutex_);

    if (front_buf_ && frame_width_ == width && frame_height_ == height) {
        return;
    }

    spdlog::debug("[CameraStream] Allocating buffers for {}x{}", width, height);

    // Retire old front buffer — LVGL may still reference it via lv_image_set_src
    // until the widget processes the next frame and updates the source pointer.
    // Retired buffers are freed in free_buffers() after the thread joins.
    if (front_buf_) {
        retired_bufs_.push_back(front_buf_);
        front_buf_ = nullptr;
    }
    if (back_buf_) {
        destroy_draw_buf(back_buf_);
        back_buf_ = nullptr;
    }

    auto w = static_cast<uint32_t>(width);
    auto h = static_cast<uint32_t>(height);
    front_buf_ = create_draw_buf(w, h, LV_COLOR_FORMAT_RGB888);
    back_buf_ = create_draw_buf(w, h, LV_COLOR_FORMAT_RGB888);

    if (!front_buf_ || !back_buf_) {
        spdlog::error("[CameraStream] Failed to allocate draw buffers for {}x{}", width, height);
        destroy_draw_buf(front_buf_);
        destroy_draw_buf(back_buf_);
        front_buf_ = nullptr;
        back_buf_ = nullptr;
        return;
    }

    frame_width_ = width;
    frame_height_ = height;
}

void CameraStream::free_buffers() {
    std::lock_guard<std::mutex> lock(buf_mutex_);
    destroy_draw_buf(front_buf_);
    front_buf_ = nullptr;
    destroy_draw_buf(back_buf_);
    back_buf_ = nullptr;
    for (auto* buf : retired_bufs_) {
        destroy_draw_buf(buf);
    }
    retired_bufs_.clear();
    transpose_buf_.reset();
    transpose_buf_size_ = 0;
    frame_width_ = 0;
    frame_height_ = 0;
}

} // namespace helix

#endif // HELIX_HAS_CAMERA
