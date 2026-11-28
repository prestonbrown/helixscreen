// SPDX-License-Identifier: GPL-3.0-or-later
#include "render_source_slot.h"

#include <spdlog/spdlog.h>

namespace helix::audio {

namespace {

// The calls this thread is inside. clear() reads it to tell whether its own
// thread holds the call it would wait for.
thread_local int t_calls_on_this_thread = 0;

} // namespace

// Ends a call however the source returned. The copy goes before the count
// comes down: what it captured is gone when clear() returns.
struct RenderSourceSlot::CallEnd {
    RenderSourceSlot& slot;
    Source& copy;

    ~CallEnd() {
        copy = nullptr;
        --t_calls_on_this_thread;
        {
            std::lock_guard<std::mutex> lock(slot.mutex_);
            --slot.calls_in_flight_;
        }
        slot.idle_cv_.notify_all();
    }
};

void RenderSourceSlot::set(Source source) {
    std::lock_guard<std::mutex> lock(mutex_);
    source_ = std::move(source);
}

void RenderSourceSlot::clear() {
    std::unique_lock<std::mutex> lock(mutex_);
    source_ = nullptr;
    if (t_calls_on_this_thread > 0) {
        spdlog::error("[RenderSourceSlot] clear() from inside the source: not waited, the call in "
                      "flight is the last");
        return;
    }
    // No call can start after the line above. The one in flight runs on its
    // own copy, and the owner frees what that copy points at once this
    // returns, so the return waits for it.
    idle_cv_.wait(lock, [this] { return calls_in_flight_ == 0; });
}

bool RenderSourceSlot::call(float* out, size_t frames, int sample_rate) {
    Source source;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        source = source_;
        if (source) {
            ++calls_in_flight_;
        }
    }
    if (!source) {
        return false;
    }
    ++t_calls_on_this_thread;
    CallEnd end{*this, source};
    source(out, frames, sample_rate);
    return true;
}

bool RenderSourceSlot::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !source_;
}

} // namespace helix::audio
