// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_render_worker.h"

#include "helix_thread.h"

#include <spdlog/spdlog.h>

#include <utility>

namespace helix {
namespace gcode {

GCodeRenderWorker::~GCodeRenderWorker() {
    stop();
}

void GCodeRenderWorker::start(StepFn step) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
        spdlog::warn("[GCode Render Worker] start() called while already running");
        return;
    }
    step_ = std::move(step);
    running_ = true;
    stopping_ = false;
    thread_ = helix::make_thread(&GCodeRenderWorker::loop, this);
}

void GCodeRenderWorker::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }
        stopping_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    have_request_ = false;
    woken_ = false;
    pending_ = RenderRequest{};
    have_ready_ = false;
}

bool GCodeRenderWorker::is_running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

void GCodeRenderWorker::post(RenderRequest request) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_ = std::move(request);
        have_request_ = true;
    }
    cv_.notify_one();
}

void GCodeRenderWorker::wake() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        woken_ = true;
    }
    cv_.notify_one();
}

bool GCodeRenderWorker::take_frame(RenderFrame& frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!have_ready_) {
        return false;
    }
    std::swap(frame, ready_);
    have_ready_ = false;
    return true;
}

void GCodeRenderWorker::set_frame_ready_callback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    frame_ready_cb_ = std::move(cb);
}

void GCodeRenderWorker::loop() {
    // Owned by this thread alone: the request being worked on and the frame
    // being drawn into.
    RenderRequest current;
    RenderFrame back;
    bool more_work = false;

    for (;;) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] { return stopping_ || have_request_ || woken_ || more_work; });
            if (stopping_) {
                break;
            }
            if (have_request_) {
                current = std::move(pending_);
                pending_ = RenderRequest{};
                have_request_ = false;
            }
            woken_ = false;
        }

        const StepOutcome out = step_(current, back);
        more_work = out.more_work;
        if (!more_work) {
            // Idle: drop the request so its file is not kept alive by a thread
            // with nothing left to draw from it.
            current = RenderRequest{};
        }

        std::function<void()> cb;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (out.published) {
                std::swap(back, ready_);
                have_ready_ = true;
            }
            if (out.published || out.notify) {
                cb = frame_ready_cb_;
            }
        }
        if (cb) {
            cb();
        }
    }
}

} // namespace gcode
} // namespace helix
