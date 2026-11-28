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

    // Declared before the thread: members are initialized in this order, and
    // the thread reads stop_ from its first instruction.
    std::atomic<bool> stop_{false};
    std::thread thread_;
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

TEST_CASE(
    "RenderSourceSlot: a clear() from inside the source ends the source without waiting for its "
    "own call",
    "[sound][render_source_slot]") {
    RenderSourceSlot slot;
    int calls = 0;
    slot.set([&](float*, size_t, int) {
        ++calls;
        slot.clear();
    });
    float buf[4] = {};

    // A wait for this call could never return.
    CHECK(slot.call(buf, 4, kRate));
    CHECK(calls == 1);
    CHECK(slot.empty());
    CHECK_FALSE(slot.call(buf, 4, kRate));
    CHECK(calls == 1);
}

// A thread can sit inside a call on one slot and clear another. Only a call
// on the slot being cleared is its own.
TEST_CASE(
    "RenderSourceSlot: a clear() of one slot from inside another slot's call waits out that slot's "
    "call",
    "[sound][render_source_slot]") {
    RenderSourceProbe probe;
    RenderSourceSlot cleared;
    RenderSourceSlot outer;
    cleared.set(probe.source());
    TightLoop loop(cleared);

    std::chrono::nanoseconds clear_wait{};
    float buf[64] = {};
    outer.set([&](float*, size_t, int) {
        const auto started = std::chrono::steady_clock::now();
        cleared.clear();
        clear_wait = std::chrono::steady_clock::now() - started;
    });
    REQUIRE(probe.wait_for_call_in_flight());

    // Inside this call on the other slot, the slot the loop drives is cleared.
    CHECK(outer.call(buf, 64, kRate));

    CHECK(within_one_call(clear_wait, probe.last_call()));
    CHECK(probe.captured_gone());
    CHECK_FALSE(probe.in_call());
}

// Two callers on one slot. A clear() asked from inside one caller's call
// cannot wait for that call, which ends only once the clear returns, and
// waits for the other's.
TEST_CASE("RenderSourceSlot: a clear() from inside a call waits out another caller's call",
          "[sound][render_source_slot]") {
    RenderSourceSlot slot;
    const auto clearer = std::this_thread::get_id();
    // The other caller's call ends only once the clear has been asked for and
    // has had time to start waiting, so the clear has that call in front of
    // it whatever the scheduling.
    std::atomic<bool> other_in_call{false};
    std::atomic<bool> clear_asked{false};
    std::atomic<bool> captured_gone{false};
    std::chrono::nanoseconds clear_wait{};
    bool other_in_call_when_clear_returned = true;
    bool captured_gone_inside_the_call = true;
    // The capture lives in the source only: the slot's copy, and the copy
    // each call runs on.
    slot.set([&slot, clearer, &other_in_call, &clear_asked, &captured_gone, &clear_wait,
              &other_in_call_when_clear_returned, &captured_gone_inside_the_call,
              captured = std::make_shared<RenderSourceProbe::Captured>(captured_gone)](
                 float*, size_t, int) {
        if (std::this_thread::get_id() != clearer) {
            other_in_call = true;
            const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!clear_asked.load() && std::chrono::steady_clock::now() < give_up) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            other_in_call = false;
            return;
        }
        const auto started = std::chrono::steady_clock::now();
        clear_asked = true;
        slot.clear();
        clear_wait = std::chrono::steady_clock::now() - started;
        // Read while this call still holds its copy of the source.
        other_in_call_when_clear_returned = other_in_call.load();
        captured_gone_inside_the_call = captured_gone.load();
    });
    TightLoop loop(slot);
    const auto in_call_by = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!other_in_call.load() && std::chrono::steady_clock::now() < in_call_by) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(other_in_call.load());

    float buf[64] = {};
    CHECK(slot.call(buf, 64, kRate));

    // The other caller's call was waited out, and this call's own copy of the
    // source lived until the call returned.
    CHECK(within_one_call(clear_wait, std::chrono::milliseconds(20)));
    CHECK_FALSE(other_in_call_when_clear_returned);
    CHECK_FALSE(captured_gone_inside_the_call);
    const auto gone_by = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!captured_gone.load() && std::chrono::steady_clock::now() < gone_by) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(captured_gone.load());
}
