// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A render source for the sound backends' clear_render_source() pins
// (prestonbrown/helixscreen#2010, prestonbrown/helixscreen#2161): each call
// takes a known time, and what the source captures reports its own
// destruction.

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>

namespace helix::test {

/// Waiting out the call in flight is bounded by that one call.
inline bool within_one_call(std::chrono::nanoseconds clear_wait,
                            std::chrono::nanoseconds last_call) {
    return clear_wait <= 2 * last_call + std::chrono::milliseconds(100);
}

/// Declare it before the backend it feeds, so it outlives whatever still runs
/// the source.
class RenderSourceProbe {
  public:
    /// The source to hand a backend. Only the returned function holds the
    /// capture, so its destruction is the backend's doing.
    [[nodiscard]] std::function<void(float*, size_t, int)> source() {
        return [this, captured = std::make_shared<Captured>(captured_gone_)](float*, size_t, int) {
            call();
        };
    }

    /// Waits until a call is in flight with a finished call behind it, so the
    /// clear has one whole call in front of it and a measured length to
    /// compare with.
    bool
    wait_for_call_in_flight(std::chrono::milliseconds limit = std::chrono::milliseconds(3000)) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while ((!in_call() || calls() < 2) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        return in_call() && last_call() > std::chrono::nanoseconds::zero();
    }

    [[nodiscard]] bool in_call() const {
        return in_call_.load();
    }
    [[nodiscard]] int calls() const {
        return calls_.load();
    }
    [[nodiscard]] std::chrono::nanoseconds last_call() const {
        return last_call_.load();
    }
    [[nodiscard]] bool captured_gone() const {
        return captured_gone_.load();
    }

    /// True when no call began in the 50 ms after @p calls_at_clear was read.
    [[nodiscard]] bool no_call_after(int calls_at_clear) const {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return calls() == calls_at_clear;
    }

    /// What a source captured, for a source of a case's own: it reports its
    /// destruction at the end of a slow death, so a clear that returned while
    /// a copy of the source still held it is caught.
    struct Captured {
        explicit Captured(std::atomic<bool>& flag) : gone(flag) {}
        ~Captured() {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            gone = true;
        }
        std::atomic<bool>& gone;
    };

  private:
    void call() const {
        const auto started = std::chrono::steady_clock::now();
        in_call_ = true;
        ++calls_;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        in_call_ = false;
        last_call_ = std::chrono::steady_clock::now() - started;
    }

    mutable std::atomic<bool> in_call_{false};
    mutable std::atomic<int> calls_{0};
    mutable std::atomic<std::chrono::nanoseconds> last_call_{std::chrono::nanoseconds::zero()};
    std::atomic<bool> captured_gone_{false};
};

} // namespace helix::test
