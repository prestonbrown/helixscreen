// SPDX-License-Identifier: GPL-3.0-or-later
#include "render_source_slot.h"

#include <spdlog/spdlog.h>

#include <utility>
#include <vector>

namespace helix::audio {

namespace {

// A call this thread is inside: the slot, and how deep when the source calls
// the slot again.
struct CallFrame {
    const RenderSourceSlot* slot;
    int depth;
};

// The calls this thread is inside, one frame per slot. clear() reads it to
// tell the calls it can end from the calls it must wait for.
thread_local std::vector<CallFrame> t_calls_on_this_thread;

int& depth_on_this_thread(const RenderSourceSlot& slot) {
    for (auto& frame : t_calls_on_this_thread) {
        if (frame.slot == &slot) {
            return frame.depth;
        }
    }
    t_calls_on_this_thread.push_back({&slot, 0});
    return t_calls_on_this_thread.back().depth;
}

} // namespace

// Ends a call however the source returned. The copy goes before the count
// comes down: what it captured is gone when clear() returns.
struct RenderSourceSlot::CallEnd {
    RenderSourceSlot& slot;
    Source& copy;

    ~CallEnd() {
        copy = nullptr;
        --depth_on_this_thread(slot);
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
    // The calls this thread is inside on this slot run on their own copies
    // and end only once this returns, so they are not waited for. Every other
    // call is: the owner frees what the source captured once this returns.
    const int own = depth_on_this_thread(*this);
    if (own > 0) {
        spdlog::error("[RenderSourceSlot] clear() from inside the source: what it captured is "
                      "not freed until the call it was asked from returns");
    }
    idle_cv_.wait(lock, [this, own] { return calls_in_flight_ <= own; });
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
    ++depth_on_this_thread(*this);
    CallEnd end{*this, source};
    source(out, frames, sample_rate);
    return true;
}

bool RenderSourceSlot::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !source_;
}

} // namespace helix::audio
