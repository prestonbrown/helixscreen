// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file bed_mesh_render_thread.h
 * @brief Double-buffered worker thread for off-screen bed mesh rendering
 *
 * Renders bed mesh frames into pixel buffers in the background so the main
 * LVGL thread can blit the ready buffer without blocking. Uses two PixelBuffers
 * (front/back) and swaps pointers after each completed render.
 *
 * Usage:
 *   BedMeshRenderThread rt;
 *   rt.set_renderer(renderer);
 *   rt.set_colors(colors);
 *   rt.set_frame_ready_callback([widget]() {
 *       helix::ui::queue_widget_update(widget, [](lv_obj_t* w) {
 *           lv_obj_invalidate(w);
 *       });
 *   });
 *   rt.start(width, height);
 *   // ... on mesh data change:
 *   rt.request_render();
 *   // ... in draw callback:
 *   if (auto* buf = rt.get_ready_buffer()) { blit(buf); }
 */

#include "bed_mesh_buffer.h"
#include "bed_mesh_renderer.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace helix {
namespace mesh {

class BedMeshRenderThread {
  public:
    BedMeshRenderThread();
    ~BedMeshRenderThread();

    // Non-copyable, non-movable
    BedMeshRenderThread(const BedMeshRenderThread&) = delete;
    BedMeshRenderThread& operator=(const BedMeshRenderThread&) = delete;
    BedMeshRenderThread(BedMeshRenderThread&&) = delete;
    BedMeshRenderThread& operator=(BedMeshRenderThread&&) = delete;

    /**
     * Start the render thread with buffer dimensions.
     * Allocates two PixelBuffers (front + back).
     */
    void start(int width, int height);

    /**
     * Stop and join the thread. Safe to call multiple times.
     * Safe to call if never started.
     */
    void stop();

    /** True if the render thread is active. */
    bool is_running() const;

    /**
     * Set the renderer to use for rendering. NOT owned by this class.
     * Must be set before requesting renders (otherwise render silently fails).
     */
    void set_renderer(bed_mesh_renderer_t* renderer);

    /**
     * Set theme colors for rendering.
     * Must be called from main thread (where theme colors are accessible).
     */
    void set_colors(const bed_mesh_render_colors_t& colors);

    /**
     * Request a new frame render.
     * Coalesces rapid requests -- only the latest matters.
     */
    void request_render();

    /** True if a rendered frame is available for reading. */
    bool has_ready_buffer() const;

    /**
     * Get the ready (front) buffer for blitting.
     * Returns nullptr if no frame has been rendered yet.
     * Valid until the next buffer swap (when the render thread finishes
     * its next frame).
     */
    const PixelBuffer* get_ready_buffer() const;

    /**
     * Set a callback invoked from the render thread when a frame is ready.
     * Typically calls helix::ui::queue_widget_update() to invalidate a widget.
     */
    void set_frame_ready_callback(std::function<void()> cb);

    /** Last frame render time in milliseconds (for adaptive quality). */
    float last_render_time_ms() const;

    /**
     * Reset adaptive quality state.
     * Call when mesh data changes (e.g., profile switch) since a different
     * mesh may render faster and should start in gradient mode.
     */
    void reset_quality();

    /**
     * Lock the renderer for main-thread modifications (rotation, dragging).
     * The render thread holds this mutex during the actual render call,
     * so the main thread must acquire it before modifying renderer state.
     */
    std::mutex& render_mutex() {
        return renderer_mutex_;
    }

    /**
     * Lock the front buffer for reading. Returns the buffer and a lock guard.
     * The buffer is guaranteed stable while the guard is alive, preventing
     * the render thread from swapping buffers during a blit.
     */
    struct LockedBuffer {
        const PixelBuffer* buffer;
        std::unique_lock<std::mutex> lock;
        explicit operator bool() const {
            return buffer != nullptr;
        }
    };
    LockedBuffer lock_ready_buffer() const;

  private:
    void render_loop();

    // Thread management
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> render_requested_{false};
    std::condition_variable cv_;
    std::mutex cv_mutex_;

    // Double buffer: front (read by main thread), back (written by render thread)
    std::unique_ptr<PixelBuffer> front_buffer_;
    std::unique_ptr<PixelBuffer> back_buffer_;
    mutable std::mutex swap_mutex_;
    std::atomic<bool> buffer_ready_{false};

    // Renderer (not owned)
    bed_mesh_renderer_t* renderer_{nullptr};
    std::mutex renderer_mutex_;

    // Colors for rendering
    bed_mesh_render_colors_t colors_{};
    std::mutex colors_mutex_;

    // Callback when frame is ready
    std::function<void()> frame_ready_callback_;
    std::mutex callback_mutex_;

    // Timing
    std::atomic<float> last_render_time_ms_{0.0f};

    // Adaptive quality degradation
    static constexpr int FRAME_HISTORY_SIZE = 5;
    static constexpr float DEGRADE_THRESHOLD_MS = 200.0f; // Switch to solid at ~5 FPS
    static constexpr float RESTORE_THRESHOLD_MS = 100.0f; // Restore gradient at ~10 FPS
    std::array<float, FRAME_HISTORY_SIZE> recent_frame_times_{};
    int frame_count_{0};
    bool degraded_mode_{false};
};

} // namespace mesh
} // namespace helix
