// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_gcode_render_worker.cpp
 * @brief The 3D preview's draw thread, driven by a fake step: no GL context.
 */

#include "gcode_render_worker.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::gcode::GCodeRenderWorker;
using helix::gcode::RenderFrame;
using helix::gcode::RenderRequest;
using Outcome = GCodeRenderWorker::StepOutcome;

namespace {

template <typename Pred> bool wait_until(Pred pred, int timeout_ms = 2000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

RenderRequest request_of_width(int width) {
    RenderRequest r;
    r.width = width;
    r.height = 1;
    return r;
}

/// A step that blocks until released, so a test can pile requests up behind it.
struct Gate {
    std::mutex m;
    std::condition_variable cv;
    bool open = false;
    std::atomic<bool> entered{false};

    void wait() {
        entered = true;
        std::unique_lock<std::mutex> lock(m);
        cv.wait(lock, [this] { return open; });
    }
    void release() {
        {
            std::lock_guard<std::mutex> lock(m);
            open = true;
        }
        cv.notify_all();
    }
};

} // namespace

TEST_CASE("render worker: a request wakes the step, and coalesced requests draw only the latest",
          "[gcode][render_worker]") {
    Gate gate;
    std::mutex seen_mutex;
    std::vector<int> seen;
    GCodeRenderWorker worker;
    worker.start([&](const RenderRequest& req, RenderFrame&) {
        {
            std::lock_guard<std::mutex> lock(seen_mutex);
            seen.push_back(req.width);
        }
        if (req.width == 1) {
            gate.wait();
        }
        return Outcome{};
    });

    worker.post(request_of_width(1));
    REQUIRE(wait_until([&] { return gate.entered.load(); }));
    // Three requests arrive while the first step is still drawing.
    worker.post(request_of_width(2));
    worker.post(request_of_width(3));
    worker.post(request_of_width(4));
    gate.release();

    REQUIRE(wait_until([&] {
        std::lock_guard<std::mutex> lock(seen_mutex);
        return seen.size() >= 2;
    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    worker.stop();
    REQUIRE(seen == std::vector<int>{1, 4});
}

TEST_CASE("render worker: a published frame is taken once, then there is no newer frame",
          "[gcode][render_worker]") {
    std::atomic<int> frames{0};
    GCodeRenderWorker worker;
    worker.set_frame_ready_callback([&] { frames++; });
    worker.start([](const RenderRequest& req, RenderFrame& back) {
        back.width = req.width;
        back.height = 1;
        back.pixels.assign(static_cast<size_t>(req.width) * 4, 0xAB);
        return Outcome{true, false, false};
    });

    RenderFrame mine;
    REQUIRE_FALSE(worker.take_frame(mine));

    worker.post(request_of_width(7));
    REQUIRE(wait_until([&] { return frames.load() == 1; }));
    REQUIRE(worker.take_frame(mine));
    REQUIRE(mine.width == 7);
    REQUIRE(mine.pixels.size() == 28);

    RenderFrame again;
    REQUIRE_FALSE(worker.take_frame(again));
    REQUIRE(again.width == 0);
    worker.stop();
}

TEST_CASE("render worker: the step keeps running while it reports more work",
          "[gcode][render_worker]") {
    std::atomic<int> calls{0};
    std::atomic<int> frames{0};
    GCodeRenderWorker worker;
    worker.set_frame_ready_callback([&] { frames++; });
    worker.start([&](const RenderRequest&, RenderFrame&) {
        const int n = ++calls;
        // Five slices of one job, then the finished frame.
        return n < 5 ? Outcome{false, true, false} : Outcome{true, false, false};
    });

    worker.post(request_of_width(1));
    REQUIRE(wait_until([&] { return frames.load() == 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    worker.stop();
    REQUIRE(calls.load() == 5);
}

TEST_CASE("render worker: stop joins a step in progress and no callback fires after it",
          "[gcode][render_worker]") {
    Gate gate;
    std::atomic<int> callbacks{0};
    std::atomic<bool> stopped{false};
    std::atomic<int> late_callbacks{0};
    GCodeRenderWorker worker;
    worker.set_frame_ready_callback([&] {
        callbacks++;
        if (stopped.load()) {
            late_callbacks++;
        }
    });
    worker.start([&](const RenderRequest&, RenderFrame&) {
        gate.wait();
        return Outcome{true, true, false};
    });

    worker.post(request_of_width(1));
    REQUIRE(wait_until([&] { return gate.entered.load(); }));

    std::thread stopper([&] {
        worker.stop();
        stopped = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    // stop() is waiting on the step, not abandoning it.
    REQUIRE_FALSE(stopped.load());
    REQUIRE(worker.is_running());
    gate.release();
    stopper.join();

    REQUIRE_FALSE(worker.is_running());
    REQUIRE(callbacks.load() <= 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    REQUIRE(late_callbacks.load() == 0);
}

TEST_CASE("render worker: the frame-ready callback runs on the worker thread",
          "[gcode][render_worker]") {
    std::mutex m;
    std::thread::id step_thread;
    std::thread::id callback_thread;
    std::atomic<bool> called{false};
    GCodeRenderWorker worker;
    worker.set_frame_ready_callback([&] {
        {
            std::lock_guard<std::mutex> lock(m);
            callback_thread = std::this_thread::get_id();
        }
        called = true;
    });
    worker.start([&](const RenderRequest&, RenderFrame&) {
        std::lock_guard<std::mutex> lock(m);
        step_thread = std::this_thread::get_id();
        return Outcome{false, false, true};
    });

    worker.post(request_of_width(1));
    REQUIRE(wait_until([&] { return called.load(); }));
    worker.stop();
    std::lock_guard<std::mutex> lock(m);
    REQUIRE(callback_thread == step_thread);
    REQUIRE(callback_thread != std::this_thread::get_id());
}
