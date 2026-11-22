// Copyright (C) 2025-2026 356C LLC
// TEST_MIRROR_OK: tests lv_timer_handler_safe() itself, test infrastructure by definition
// SPDX-License-Identifier: GPL-3.0-or-later
//
// lv_async_call's own timer wrapper (lib/lvgl/src/misc/lv_async.c#lv_async_timer_cb)
// deletes its lv_timer_t node before invoking the caller's function, so a
// second lv_async_call made from inside that function can allocate its new
// timer node at the address the first one just vacated (every lv_timer_t
// comes from the same fixed-size free list, lib/lvgl/src/misc/lv_ll.c).
// lv_timer_handler_safe() reaps an exhausted one-shot by its CURRENT
// repeat_count on a fresh list walk, never by a pointer saved before running
// its callback, so a reused address is never mistaken for the timer that used
// to be there.
//
// Mutation check: reintroduce a re-find-by-pointer step after a one-shot's
// callback runs (look up the same `lv_timer_t*` again and delete it) and the
// first test below goes red.

#include "../lvgl_test_fixture.h"
#include "../test_helpers/process_async_timers.h"
#include "../ui_test_utils.h"
#include "lvgl/lvgl.h"

#include <algorithm>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

struct AbaProbe {
    lv_timer_t* outer_addr = nullptr;
    bool address_reused = false;
    bool inner_ran = false;
};

void inner_cb(void* user_data) {
    static_cast<AbaProbe*>(user_data)->inner_ran = true;
}

void outer_cb(void* user_data) {
    auto* probe = static_cast<AbaProbe*>(user_data);
    // By the time this runs, lv_async_timer_cb has already deleted the timer
    // node that carried this call (the file comment above). Scheduling the
    // nested call here is what lets its allocation land on that freed slot
    // before lv_timer_handler_safe() gets a chance to look again.
    lv_async_call(inner_cb, probe);
    for (lv_timer_t* t = lv_timer_get_next(nullptr); t != nullptr; t = lv_timer_get_next(t)) {
        if (t == probe->outer_addr) {
            probe->address_reused = true;
            break;
        }
    }
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "lv_timer_handler_safe delivers a nested lv_async_call after its timer's "
                 "address is reused (ABA)",
                 "[core][fixture][timer]") {
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
    SKIP("ASan quarantines freed memory, so the freed timer address is never handed back");
#endif
    AbaProbe probe;

    std::vector<lv_timer_t*> before;
    for (lv_timer_t* t = lv_timer_get_next(nullptr); t != nullptr; t = lv_timer_get_next(t)) {
        before.push_back(t);
    }

    lv_async_call(outer_cb, &probe);

    // The one timer address that appeared as a side effect of the call above
    // is the node lv_async_call just created for it.
    for (lv_timer_t* t = lv_timer_get_next(nullptr); t != nullptr; t = lv_timer_get_next(t)) {
        if (std::find(before.begin(), before.end(), t) == before.end()) {
            probe.outer_addr = t;
            break;
        }
    }
    REQUIRE(probe.outer_addr != nullptr);

    lv_timer_handler_safe();

    // Confirms the ABA precondition this test relies on actually occurred. If
    // a future LVGL allocator change stopped reusing the address, this fails
    // here with a clear reason instead of the assertion below passing for the
    // wrong one.
    REQUIRE(probe.address_reused);

    CHECK(probe.inner_ran);
}

namespace {

struct CollateralProbe {
    lv_timer_t* victim = nullptr;
};

void deletes_a_different_timer(lv_timer_t* self) {
    auto* probe = static_cast<CollateralProbe*>(lv_timer_get_user_data(self));
    lv_timer_delete(probe->victim);
}

bool timer_list_contains(lv_timer_t* target) {
    for (lv_timer_t* t = lv_timer_get_next(nullptr); t != nullptr; t = lv_timer_get_next(t)) {
        if (t == target) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "lv_timer_handler_safe still reaps a timer whose own callback deleted a "
                 "different one",
                 "[core][fixture][timer]") {
    // A stand-in for lv_async_call_cancel() or any of the several
    // lv_timer_delete(other_timer) call sites in the app (wifi_manager.cpp,
    // spoolman_manager.cpp, ui_toast_manager.cpp): a timer's own callback
    // deletes an unrelated timer, not itself.
    CollateralProbe probe;
    probe.victim = lv_timer_create([](lv_timer_t*) {}, 1000, nullptr);
    REQUIRE(probe.victim != nullptr);
    lv_timer_set_repeat_count(probe.victim, -1); // never fires; only a delete target

    lv_timer_t* caller = lv_timer_create(deletes_a_different_timer, 0, &probe);
    REQUIRE(caller != nullptr);
    lv_timer_set_repeat_count(caller, 1);

    lv_timer_handler_safe();

    // The victim: deleted directly by the callback above.
    CHECK_FALSE(timer_list_contains(probe.victim));
    // The caller: never touched itself, but its own repeat_count reached
    // zero running that callback, so it must be reaped too, not left leaking
    // its (now dangling) `probe` pointer in LVGL's list forever.
    CHECK_FALSE(timer_list_contains(caller));
}

namespace {

int g_reentrant_user_fn_calls = 0;

void reentrant_user_fn(void*) {
    g_reentrant_user_fn_calls++;
    // Pumps through both entry points a real caller could reach from inside
    // an lv_async_call's own user function: the test harness's pump, and
    // LVGL's own handler directly.
    lv_timer_handler_safe();
    lv_tick_inc(1);
    lv_timer_handler();
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "lv_timer_handler_safe survives a self-deleting async call whose function "
                 "pumps timers again",
                 "[core][fixture][timer]") {
    g_reentrant_user_fn_calls = 0;

    lv_async_call(reentrant_user_fn, nullptr);
    lv_timer_handler_safe();

    CHECK(g_reentrant_user_fn_calls == 1);
}

namespace {

void count_fire(lv_timer_t* t) {
    ++*static_cast<int*>(lv_timer_get_user_data(t));
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "fire_timer_once keeps a spent one-shot its owner holds",
                 "[core][fixture][timer]") {
    int fires = 0;
    lv_timer_t* kept = lv_timer_create(count_fire, 1000, &fires);
    lv_timer_set_repeat_count(kept, 1);
    lv_timer_set_auto_delete(kept, false);

    REQUIRE(helix::test::fire_timer_once(kept));
    CHECK(fires == 1);
    REQUIRE(timer_is_live(kept));
    CHECK(kept->paused);
    CHECK(kept->repeat_count == 0);

    CHECK_FALSE(helix::test::fire_timer_once(kept));
    CHECK(fires == 1);
    lv_timer_delete(kept);
}

TEST_CASE_METHOD(LVGLTestFixture, "fire_timer_once runs only what lv_timer_exec would",
                 "[core][fixture][timer]") {
    int fires = 0;
    lv_timer_t* t = lv_timer_create(count_fire, 1000, &fires);
    lv_timer_set_repeat_count(t, 1);

    SECTION("a spent one-shot that deletes itself is gone") {
        REQUIRE(helix::test::fire_timer_once(t));
        CHECK(fires == 1);
        CHECK_FALSE(timer_is_live(t));
        t = nullptr;
    }
    SECTION("a paused timer does not run") {
        lv_timer_pause(t);
        CHECK_FALSE(helix::test::fire_timer_once(t));
        CHECK(t->repeat_count == 1);
    }
    SECTION("a timer with no callback does not run") {
        lv_timer_set_cb(t, nullptr);
        CHECK_FALSE(helix::test::fire_timer_once(t));
        CHECK(t->repeat_count == 1);
    }
    SECTION("a periodic timer runs and stays armed") {
        lv_timer_set_repeat_count(t, -1);
        REQUIRE(helix::test::fire_timer_once(t));
        CHECK(timer_is_live(t));
        CHECK_FALSE(t->paused);
        CHECK(t->repeat_count == -1);
    }
    CHECK(fires <= 1);
    if (t) {
        lv_timer_delete(t);
    }
}
