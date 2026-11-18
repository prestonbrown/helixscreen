// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file gcode_render_worker.h
 * @brief The thread the 3D G-code preview draws on, without the GL.
 *
 * The LVGL thread posts what it wants drawn (post()) and takes the newest
 * finished frame (take_frame()); neither call waits on a draw. The worker
 * runs a step function, supplied by the renderer, for the latest request and
 * keeps calling it while the step reports more work (an upload or a
 * refinement in progress), with no new request needed.
 *
 * Three frames rotate: the worker writes `back`, a finished frame waits in
 * `ready`, and the consumer holds the one it took. take_frame() swaps the
 * consumer's frame with `ready`, so pixels are never copied here and the
 * worker never writes a frame the consumer holds.
 *
 * No LVGL in here: the frame-ready callback runs on the worker thread, and the
 * consumer is responsible for hopping to the LVGL thread from it.
 */

#include "gcode_camera.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <glm/glm.hpp>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace helix {
namespace gcode {

struct ParsedGCodeFile;

/// What the LVGL thread asks the worker to draw.
struct RenderRequest {
    GCodeCamera camera;
    int width = 0;  ///< widget width the frame is displayed at
    int height = 0; ///< widget height the frame is displayed at
    /// Held for as long as the worker draws from it, so the viewer can drop or
    /// replace its file at any time.
    std::shared_ptr<const ParsedGCodeFile> gcode;
};

/// A finished frame: plain memory, no LVGL objects.
struct RenderFrame {
    std::vector<uint8_t> pixels; ///< ARGB8888, rows of width * 4 bytes, top row first
    int width = 0;
    int height = 0;
    glm::mat4 mvp{1.0f};     ///< the transform the frame was drawn with
    uint64_t generation = 0; ///< the renderer's scene generation when it was drawn
};

class GCodeRenderWorker {
  public:
    struct StepOutcome {
        bool published = false; ///< `back` holds a finished frame to hand to the consumer
        bool more_work = false; ///< call the step again without waiting for a request
        bool notify = false;    ///< run the frame-ready callback even without a frame
    };
    /// Runs on the worker thread with the latest request and the frame to draw into.
    using StepFn = std::function<StepOutcome(const RenderRequest&, RenderFrame&)>;

    GCodeRenderWorker() = default;
    ~GCodeRenderWorker();

    GCodeRenderWorker(const GCodeRenderWorker&) = delete;
    GCodeRenderWorker& operator=(const GCodeRenderWorker&) = delete;

    /// Start the thread. A second start() while running is ignored.
    void start(StepFn step);

    /// Wake the thread, let a step in progress finish, and join. No callback
    /// runs after this returns. Safe without start() and safe twice.
    void stop();

    bool is_running() const;

    /// Replace the pending request (only the latest one is ever drawn) and wake the worker.
    void post(RenderRequest request);

    /// Run the step once more without a new request, e.g. to free GPU memory.
    void wake();

    /// Consumer side: when a frame was published since the last call, swap it
    /// into `frame` (which hands `frame`'s old buffer back to the worker) and
    /// return true. Otherwise leave `frame` alone and return false.
    bool take_frame(RenderFrame& frame);

    /// Called on the worker thread after each published frame (and each
    /// StepOutcome::notify). Must not touch LVGL.
    void set_frame_ready_callback(std::function<void()> cb);

  private:
    void loop();

    std::thread thread_;
    StepFn step_;

    mutable std::mutex mutex_; ///< guards everything below
    std::condition_variable cv_;
    bool running_ = false;
    bool stopping_ = false;
    bool have_request_ = false;
    bool woken_ = false;
    RenderRequest pending_;
    RenderFrame ready_;
    bool have_ready_ = false;
    std::function<void()> frame_ready_cb_;
};

} // namespace gcode
} // namespace helix
