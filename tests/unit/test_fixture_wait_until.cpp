// Copyright (C) 2025-2026 356C LLC
// TEST_MIRROR_OK: tests the LVGLTestFixture contract itself, which is test infrastructure by
// definition SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_fixture_wait_until.cpp
 * @brief Pins the contract of LVGLTestFixture::wait_until() vs process_lvgl()
 *
 * process_lvgl() advances LVGL's VIRTUAL clock and never sleeps. A wall-clock
 * wait built out of it burns its whole budget at once and never yields to the
 * thread it is watching, so the test fails past the end of the fixture's
 * lifetime and teardown looks like it happened mid-test.
 *
 * wait_until() is the real-time wait, and it has to get BOTH halves right: a real
 * steady_clock deadline with real sleeps (so other threads run), and
 * lv_tick_inc() each pass (so LVGL's clock moves at all — the test binary's
 * display has no driver, hence no tick callback, so nothing else advances it).
 */

#include "ui_test_utils.h"

#include "../lvgl_test_fixture.h"

#include <atomic>
#include <chrono>
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace std::chrono;

namespace {

/// Elapsed REAL milliseconds while running fn.
template <typename F> long long real_ms(F&& fn) {
    const auto t0 = steady_clock::now();
    fn();
    return duration_cast<milliseconds>(steady_clock::now() - t0).count();
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "process_lvgl advances virtual time, not real time",
                 "[core][fixture][timing]") {
    // The whole reason wait_until() has to exist. If this ever starts sleeping
    // proportionally, the wall-clock-wait trap is gone and the docs are stale.
    const uint32_t tick_before = lv_tick_get();
    long long elapsed = real_ms([&] { process_lvgl(500); });

    // Virtual time advanced by the full nominal amount. Asserted on the FIRST
    // sample, before any retry below adds another 500 ticks.
    CHECK(lv_tick_get() - tick_before == 500);

    // ...while real time did not: a few milliseconds of timer CPU, against the
    // 100ms that even a 1ms sleep per 5ms step would cost. Other shards compete
    // for the same cores, so one descheduled sample can miss the bound with the
    // contract intact; a second sample keeps the bound strict, since a
    // process_lvgl that sleeps proportionally misses both.
    if (elapsed >= 50) {
        elapsed = real_ms([&] { process_lvgl(500); });
    }
    // Timers an earlier test left running are CPU, not sleep: charge them to neither.
    const long long timers_alone = real_ms([] {
        for (int i = 0; i < 100; ++i) {
            lv_tick_inc(5);
            lv_timer_handler_safe();
        }
    });
    CHECK(elapsed < timers_alone + 50);
}

TEST_CASE_METHOD(LVGLTestFixture, "wait_until observes a value published by another thread",
                 "[core][fixture][timing]") {
    std::atomic<bool> published{false};
    std::thread worker([&] {
        std::this_thread::sleep_for(milliseconds(120));
        published.store(true);
    });

    const bool ok = wait_until([&] { return published.load(); }, 5000);
    worker.join();

    // Fails with a non-yielding wait: the predicate is polled a few thousand
    // times in the first millisecond and then the budget is gone.
    CHECK(ok);
}

TEST_CASE_METHOD(LVGLTestFixture, "wait_until times out on the REAL clock",
                 "[core][fixture][timing]") {
    long long elapsed = 0;
    bool ok = true;
    elapsed = real_ms([&] { ok = wait_until([] { return false; }, 200); });

    CHECK_FALSE(ok);
    // The point of the helper: a 200ms budget costs 200ms of wall time, so the
    // thread being waited on actually gets to run. A virtual-time wait returns
    // in single-digit milliseconds here.
    CHECK(elapsed >= 180);
}

TEST_CASE_METHOD(LVGLTestFixture, "wait_until advances the virtual clock so timers come due",
                 "[core][fixture][timing]") {
    // The mirror-image trap: a wait that sleeps on the real clock but never
    // calls lv_tick_inc() leaves LVGL frozen. lv_timer_handler compares
    // `now - last_run >= period`, so a periodic timer never becomes ready and
    // this test hangs to its timeout.
    static std::atomic<int> fired{0};
    fired.store(0);

    lv_timer_t* timer = lv_timer_create([](lv_timer_t*) { fired.fetch_add(1); }, 30, nullptr);
    REQUIRE(timer != nullptr);
    // lv_timer_handler_safe() only runs timers with repeat_count > 0; the
    // default of -1 (infinite) would be skipped entirely.
    lv_timer_set_repeat_count(timer, 1);

    const bool ok = wait_until([] { return fired.load() > 0; }, 2000);

    CHECK(ok);
    CHECK(fired.load() == 1);

    // lv_timer_handler_safe() now reaps a timer whose repeat count reached 0,
    // exactly as lv_timer_handler() does (lv_timer.c). Deleting it here would be
    // a double free. Assert the reap instead: an exhausted one-shot left in the
    // list keeps its callback's user_data alive past its owner, which is how a
    // spent timer ends up firing on freed memory in a later test.
    for (lv_timer_t* t = lv_timer_get_next(nullptr); t != nullptr; t = lv_timer_get_next(t)) {
        CHECK(t != timer);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "wait_until evaluates its condition at least once",
                 "[core][fixture][timing]") {
    // A zero timeout must still get one full pump-and-check pass, otherwise an
    // already-satisfied condition reports failure.
    int calls = 0;
    const bool ok = wait_until(
        [&] {
            calls++;
            return true;
        },
        0);

    CHECK(ok);
    CHECK(calls == 1);
}
