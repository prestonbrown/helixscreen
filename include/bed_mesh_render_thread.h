// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file bed_mesh_render_thread.h
 * @brief Double-buffered worker thread for off-screen bed mesh rendering
 *
 * Renders bed mesh frames into pixel buffers in the background so the main
 * LVGL thread can blit the ready buffer without blocking. Exactly two
 * PixelBuffers exist; ownership moves between the render thread and the
 * consumer by pointer swap under swap_mutex_.
 *
 * Usage:
 *   BedMeshRenderThread rt;
 *   rt.set_renderer(renderer);
 *   rt.set_colors(colors);
 *   rt.set_frame_ready_callback([widget = helix::ui::WeakWidget(widget)]() {
 *       helix::ui::queue_widget_update(widget, [](lv_obj_t* w) {
 *           lv_obj_invalidate(w);
 *       });
 *   });
 *   rt.start(width, height);
 *   // ... on mesh data change:
 *   rt.request_render();
 *   // ... in draw callback:
 *   if (auto* buf = rt.acquire_frame()) { blit(buf); }
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
     * Allocates the two PixelBuffers.
     */
    void start(int width, int height);

    /**
     * Stop and join the thread, then free both buffers. Safe to call multiple times.
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

    /** True once the render thread has published at least one frame since start(). */
    bool has_ready_buffer() const;

    /**
     * Consumer side (main thread only). Takes the newest published frame, if
     * any, by swapping it with the buffer the consumer was showing; the old
     * buffer goes back to the render thread. Without a new frame nothing moves
     * and the same pointer is returned.
     *
     * Returns nullptr until the first frame has been taken. The returned buffer
     * is owned by the consumer and never written by the render thread, so it
     * stays valid and unchanged until the next acquire_frame() or stop().
     */
    const PixelBuffer* acquire_frame();

    /** Buffers currently allocated (2 while started, 0 otherwise). */
    int resident_buffer_count() const;

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

  private:
    void render_loop();
    void return_target(std::unique_ptr<PixelBuffer> buf);

    // Thread management
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> render_requested_{false};
    std::condition_variable cv_;
    std::mutex cv_mutex_;

    // Two buffers, each owned by exactly one party at a time (guarded by swap_mutex_):
    //   back_  - idle, the render thread's next target (null while it renders or a
    //            finished frame waits in ready_)
    //   ready_ - finished frame waiting for the consumer
    //   shown_ - what the consumer is reading; the render thread never touches it
    // The render thread moves back_ out, renders without the lock, then moves it
    // into ready_. acquire_frame() swaps ready_ with shown_ and returns the old
    // shown_ to back_. With no new frame, acquire_frame() copies nothing.
    std::unique_ptr<PixelBuffer> back_;
    std::unique_ptr<PixelBuffer> ready_;
    std::unique_ptr<PixelBuffer> shown_;
    mutable std::mutex swap_mutex_;
    std::atomic<bool> back_available_{false};
    std::atomic<bool> buffer_ready_{false};
    bool shown_has_frame_{false}; // main thread only

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
    static constexpr int kFrameHistorySize = 5;
    static constexpr float kDegradeThresholdMs = 200.0f; // Switch to solid at ~5 FPS
    static constexpr float kRestoreThresholdMs = 100.0f; // Restore gradient at ~10 FPS
    std::array<float, kFrameHistorySize> recent_frame_times_{};
    int frame_count_{0};
    bool degraded_mode_{false};
};

} // namespace mesh
} // namespace helix
