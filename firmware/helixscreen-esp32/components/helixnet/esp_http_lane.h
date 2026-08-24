// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// EspHttpLane — Task 10's HTTP lane: one dedicated worker pthread + a small
// bounded queue, the ESP32 equivalent of desktop's HttpExecutor lane
// (include/http_executor.h). Used by esp_rest_api.cpp's MoonrakerFileTransferAPI
// implementation for the print-select thumbnail/gcode-header fetch path.
//
// Threading model: submit_get() may be called from any thread (mirrors
// HttpExecutor::submit()). The worker thread invokes on_success/on_error
// DIRECTLY on itself — same contract as desktop's HttpExecutor lanes (see
// moonraker_file_transfer_api.cpp, which calls on_success() straight from the
// HttpExecutor worker with no queue_update wrapping). This class has zero LVGL
// dependency; callers that touch UI/subjects/`this` from their callback remain
// responsible for hopping to the main thread via ui_queue_update()/tok.defer()
// themselves (CLAUDE.md § Threading).

#pragma once

#include "http_lane_queue.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>

namespace helix::http {

// Called with a pointer into the lane's PSRAM accumulation buffer — valid
// ONLY for the duration of the callback. Copy out what you need; the buffer
// is freed the moment the callback returns.
using FetchSuccessCb = std::function<void(const uint8_t* data, size_t size)>;
using FetchErrorCb = std::function<void(const std::string& message)>;

class EspHttpLane {
  public:
    static EspHttpLane& instance();

    // Submits a capped, in-memory GET (HTTP Range: bytes=0-{cap-1}, cap =
    // clamp_fetch_cap(range_max_bytes)). Returns false immediately — queuing
    // nothing, calling neither callback — if the queue is already at
    // QUEUE_DEPTH or the worker pthread failed to start. Callers MUST NOT
    // block or retry-loop on a false return; a full queue means "try again
    // later" (e.g. next scroll tick), never a caller-side spin.
    bool submit_get(std::string url, size_t range_max_bytes, FetchSuccessCb on_success,
                    FetchErrorCb on_error);

    static constexpr size_t QUEUE_DEPTH = 8;

    EspHttpLane(const EspHttpLane&) = delete;
    EspHttpLane& operator=(const EspHttpLane&) = delete;

  private:
    EspHttpLane() = default;

    struct Job {
        std::string url;
        size_t cap = 0;
        FetchSuccessCb on_success;
        FetchErrorCb on_error;
    };

    // Caller must already hold mutex_ — mutex_ is a plain std::mutex, so this
    // must never take it itself. Returns whether the worker is running.
    bool ensure_worker_started_locked();
    static void* worker_main(void* self);
    void worker_loop();
    void run_one(const Job& job);

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> queue_;                 // guarded by mutex_
    BoundedSlotCounter slots_{QUEUE_DEPTH}; // guarded by mutex_
    bool worker_started_ = false;           // guarded by mutex_
};

} // namespace helix::http
