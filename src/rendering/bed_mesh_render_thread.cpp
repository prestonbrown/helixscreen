// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_BED_MESH_3D

#include "bed_mesh_render_thread.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>

namespace helix {
namespace mesh {

BedMeshRenderThread::BedMeshRenderThread() = default;

BedMeshRenderThread::~BedMeshRenderThread() {
    stop();
}

void BedMeshRenderThread::start(int width, int height) {
    if (running_.load()) {
        spdlog::warn("[BedMeshRenderThread] start() called while already running");
        return;
    }

    // Allocate double buffers
    front_buffer_ = std::make_unique<PixelBuffer>(width, height);
    back_buffer_ = std::make_unique<PixelBuffer>(width, height);
    buffer_ready_.store(false);
    render_requested_.store(false);
    last_render_time_ms_.store(0.0f);

    running_.store(true);
    thread_ = std::thread(&BedMeshRenderThread::render_loop, this);

    spdlog::info("[BedMeshRenderThread] Started ({}x{}, double-buffered)", width, height);
}

void BedMeshRenderThread::stop() {
    if (!running_.load()) {
        return;
    }

    spdlog::info("[BedMeshRenderThread] Stopping...");

    // Set running_ under cv_mutex_ so the render loop cannot miss the wakeup.
    // Without the lock, the store + notify can interleave between the render
    // loop's predicate evaluation and its actual sleep on the condvar, leaving
    // the thread waiting forever and hanging thread_.join().
    {
        std::lock_guard<std::mutex> lock(cv_mutex_);
        running_.store(false);
    }
    cv_.notify_all();

    if (thread_.joinable()) {
        thread_.join();
    }

    spdlog::info("[BedMeshRenderThread] Stopped");
}

bool BedMeshRenderThread::is_running() const {
    return running_.load();
}

void BedMeshRenderThread::set_renderer(bed_mesh_renderer_t* renderer) {
    std::lock_guard<std::mutex> lock(renderer_mutex_);
    renderer_ = renderer;
}

void BedMeshRenderThread::set_colors(const bed_mesh_render_colors_t& colors) {
    std::lock_guard<std::mutex> lock(colors_mutex_);
    colors_ = colors;
}

void BedMeshRenderThread::request_render() {
    {
        std::lock_guard<std::mutex> lock(cv_mutex_);
        render_requested_.store(true);
    }
    cv_.notify_one();
}

bool BedMeshRenderThread::has_ready_buffer() const {
    return buffer_ready_.load();
}

const PixelBuffer* BedMeshRenderThread::get_ready_buffer() const {
    if (!buffer_ready_.load()) {
        return nullptr;
    }
    // Front buffer is safe to read: render thread writes to back buffer.
    // The swap_mutex_ protects pointer swap, but between swaps the front
    // pointer is stable.
    return front_buffer_.get();
}

BedMeshRenderThread::LockedBuffer BedMeshRenderThread::lock_ready_buffer() const {
    if (!buffer_ready_.load()) {
        return {nullptr, {}};
    }
    std::unique_lock<std::mutex> lock(swap_mutex_);
    return {front_buffer_.get(), std::move(lock)};
}

void BedMeshRenderThread::set_frame_ready_callback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    frame_ready_callback_ = std::move(cb);
}

float BedMeshRenderThread::last_render_time_ms() const {
    return last_render_time_ms_.load();
}

void BedMeshRenderThread::reset_quality() {
    // Protect adaptive quality fields (frame_count_, recent_frame_times_,
    // degraded_mode_) which are also read/written by the render thread
    // under renderer_mutex_.
    std::lock_guard<std::mutex> lock(renderer_mutex_);

    frame_count_ = 0;
    recent_frame_times_.fill(0.0f);

    if (degraded_mode_) {
        degraded_mode_ = false;

        // Restore gradient mode on the renderer
        if (renderer_) {
            bed_mesh_renderer_set_dragging(renderer_, false);
        }
        spdlog::debug("[BedMeshRenderThread] Quality reset (gradient mode restored)");
    }
}

void BedMeshRenderThread::render_loop() {
    spdlog::debug("[BedMeshRenderThread] Render loop started");

    while (running_.load()) {
        // Wait for a render request or stop signal
        {
            std::unique_lock<std::mutex> lock(cv_mutex_);
            cv_.wait(lock, [this]() { return render_requested_.load() || !running_.load(); });
        }

        if (!running_.load()) {
            break;
        }

        // Consume the request (coalesces multiple requests into one render)
        render_requested_.store(false);

        // Snapshot colors under lock
        bed_mesh_render_colors_t colors;
        {
            std::lock_guard<std::mutex> lock(colors_mutex_);
            colors = colors_;
        }

        // Hold renderer_mutex_ during the render call and adaptive quality tracking.
        // The main thread acquires this mutex before modifying renderer state
        // (rotation, dragging), preventing concurrent access.
        bool ok = false;
        float elapsed_ms = 0.0f;
        {
            std::lock_guard<std::mutex> lock(renderer_mutex_);

            if (!renderer_) {
                spdlog::warn("[BedMeshRenderThread] Render requested but no renderer set");
                continue;
            }

            // Render into back buffer
            auto t0 = std::chrono::steady_clock::now();
            ok = bed_mesh_renderer_render_to_buffer(renderer_, *back_buffer_, colors);
            auto t1 = std::chrono::steady_clock::now();

            if (!ok) {
                spdlog::warn("[BedMeshRenderThread] render_to_buffer failed");
                continue;
            }

            elapsed_ms = std::chrono::duration<float, std::milli>(t1 - t0).count();
            last_render_time_ms_.store(elapsed_ms);

            // Track frame times for adaptive quality degradation
            recent_frame_times_[frame_count_ % FRAME_HISTORY_SIZE] = elapsed_ms;
            frame_count_++;

            if (frame_count_ >= 3) {
                int count = std::min(frame_count_, FRAME_HISTORY_SIZE);
                float avg = 0.0f;
                for (int i = 0; i < count; i++) {
                    avg += recent_frame_times_[i];
                }
                avg /= static_cast<float>(count);

                if (!degraded_mode_ && avg > DEGRADE_THRESHOLD_MS) {
                    degraded_mode_ = true;
                    bed_mesh_renderer_set_dragging(renderer_, true);
                    spdlog::info(
                        "[BedMeshRenderThread] Degrading to solid-color mode (avg {:.0f}ms)", avg);
                } else if (degraded_mode_ && avg < RESTORE_THRESHOLD_MS) {
                    degraded_mode_ = false;
                    bed_mesh_renderer_set_dragging(renderer_, false);
                    spdlog::info("[BedMeshRenderThread] Restored gradient mode (avg {:.0f}ms)",
                                 avg);
                }
            }
        } // renderer_mutex_ released

        // Swap front/back buffers
        {
            std::lock_guard<std::mutex> lock(swap_mutex_);
            front_buffer_.swap(back_buffer_);
        }
        buffer_ready_.store(true);

        spdlog::debug("[BedMeshRenderThread] Frame rendered in {:.1f} ms", elapsed_ms);

        // Notify callback (typically queues a widget invalidation)
        std::function<void()> cb;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            cb = frame_ready_callback_;
        }
        if (cb) {
            cb();
        }
    }

    spdlog::debug("[BedMeshRenderThread] Render loop exiting");
}

} // namespace mesh
} // namespace helix

#endif // HELIX_HAS_BED_MESH_3D
