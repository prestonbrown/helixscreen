// SPDX-License-Identifier: GPL-3.0-or-later
// The render source slot the sound backends share (include/render_source_slot.h):
// what clear() promises the owner (prestonbrown/helixscreen#2010), and that it
// keeps the promise without holding a lock across a call
// (prestonbrown/helixscreen#2161). Each backend's own cases pin that it uses
// the slot.

#include "../test_helpers/render_source_probe.h"
#include "render_source_slot.h"

#include <atomic>
#include <chrono>
#include <thread>

#include "../catch_amalgamated.hpp"

namespace {

using helix::audio::RenderSourceSlot;
using helix::test::RenderSourceProbe;
using helix::test::within_one_call;

constexpr int kRate = 44100;

/// Calls the slot back to back on its own thread: the shape of a render loop
/// on a device that never blocks.
class TightLoop {
  public:
    explicit TightLoop(RenderSourceSlot& slot) : thread_([this, &slot] { run(slot); }) {}
    ~TightLoop() {
        stop();
    }
    TightLoop(const TightLoop&) = delete;
    TightLoop& operator=(const TightLoop&) = delete;
    void stop() {
        stop_ = true;
        if (thread_.joinable()) {
            thread_.join();
        }
    }

  private:
    void run(RenderSourceSlot& slot) {
        float buf[64];
        while (!stop_.load()) {
            slot.call(buf, 64, kRate);
        }
    }

    std::thread thread_;
    std::atomic<bool> stop_{false};
};

} // namespace

TEST_CASE("RenderSourceSlot: clear() returns within one call under a loop that never blocks",
          "[sound][render_source_slot][2161]") {
    RenderSourceProbe probe;
    RenderSourceSlot slot;
    slot.set(probe.source());
    TightLoop loop(slot);

    // A finished call behind it and one in flight: the clear has one whole
    // call in front of it and a measured length to compare with.
    REQUIRE(probe.wait_for_call_in_flight());

    const auto clear_started = std::chrono::steady_clock::now();
    slot.clear();
    const auto clear_wait = std::chrono::steady_clock::now() - clear_started;
    CHECK(within_one_call(clear_wait, probe.last_call()));

    // The owner frees what the source captured the moment clear() returns.
    CHECK(probe.captured_gone());
    CHECK_FALSE(probe.in_call());
    const int calls_at_clear = probe.calls();
    CHECK(probe.no_call_after(calls_at_clear));

    // Nothing runs once the slot is cleared, though the loop keeps asking.
    float buf[4] = {};
    CHECK_FALSE(slot.call(buf, 4, kRate));
    CHECK(slot.empty());
}

TEST_CASE("RenderSourceSlot: call() runs the source, and set() names what the next call runs",
          "[sound][render_source_slot]") {
    RenderSourceSlot slot;
    float buf[4] = {};
    CHECK_FALSE(slot.call(buf, 4, kRate));
    CHECK(slot.empty());

    int heard_rate = 0;
    slot.set([&](float* out, size_t frames, int rate) {
        heard_rate = rate;
        for (size_t i = 0; i < frames; ++i) {
            out[i] = 1.0f;
        }
    });
    CHECK(slot.call(buf, 4, kRate));
    CHECK(heard_rate == kRate);
    CHECK(buf[3] == 1.0f);
    CHECK_FALSE(slot.empty());

    // A source that set() replaced before any call never runs.
    int replaced_runs = 0;
    slot.set([&](float* out, size_t frames, int) {
        ++replaced_runs;
        for (size_t i = 0; i < frames; ++i) {
            out[i] = 0.5f;
        }
    });
    slot.set([&](float* out, size_t frames, int) {
        for (size_t i = 0; i < frames; ++i) {
            out[i] = 0.25f;
        }
    });
    CHECK(slot.call(buf, 4, kRate));
    CHECK(buf[3] == 0.25f);
    CHECK(replaced_runs == 0);

    slot.clear();
    CHECK(slot.empty());
}

TEST_CASE("RenderSourceSlot: a clear() from inside the source ends the source without waiting",
          "[sound][render_source_slot]") {
    RenderSourceSlot slot;
    int calls = 0;
    slot.set([&](float*, size_t, int) {
        ++calls;
        slot.clear();
    });
    float buf[4] = {};

    // A wait here could never return: this call is what it waits for.
    CHECK(slot.call(buf, 4, kRate));
    CHECK(calls == 1);
    CHECK(slot.empty());
    CHECK_FALSE(slot.call(buf, 4, kRate));
    CHECK(calls == 1);
}
