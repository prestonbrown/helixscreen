// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/update_queue.h"
#include "lvgl_test_fixture.h"
#include "refresh_period_hold.h"
#include "refresh_timing_env.h"
#include "test_helpers/refresh_period_hold_test_access.h"
#include "test_helpers/scoped_env.h"
#include "test_helpers/update_queue_test_access.h"

#include <cstdlib>
#include <optional>

#include "../../catch_amalgamated.hpp"

using helix::anim_timer_period;
using helix::default_refr_timer_period;
using helix::RefreshPeriodHold;
using helix::RefreshPeriodHoldTestAccess;
using helix::ScopedEnv;
using helix::ScopedTimerPeriods;

namespace {

void noop_read(lv_indev_t* /*indev*/, lv_indev_data_t* data) {
    data->state = LV_INDEV_STATE_RELEASED;
}

uint32_t read_timer_period(lv_indev_t* indev) {
    const lv_timer_t* t = lv_indev_get_read_timer(indev);
    return t ? t->period : 0;
}

/// An input device that exists for the life of the object.
struct ScopedIndev {
    lv_indev_t* indev;
    ScopedIndev() : indev(lv_indev_create()) {
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, noop_read);
    }
    ~ScopedIndev() {
        lv_indev_delete(indev);
    }
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "RefreshPeriodHold sets the refresh and animation timers until the last release",
                 "[application][display][refresh_period]") {
    ScopedTimerPeriods restore;
    ScopedTimerPeriods::set(40, 45);
    RefreshPeriodHold hold;
    hold.set_period(16);

    hold.acquire();
    CHECK(hold.is_held());
    CHECK(default_refr_timer_period() == 16);
    CHECK(anim_timer_period() == 16);

    hold.acquire();
    hold.release();
    CHECK(hold.is_held());
    CHECK(default_refr_timer_period() == 16);
    CHECK(anim_timer_period() == 16);

    hold.release();
    CHECK_FALSE(hold.is_held());
    CHECK(default_refr_timer_period() == 40);
    CHECK(anim_timer_period() == 45);
}

TEST_CASE_METHOD(LVGLTestFixture, "RefreshPeriodHold ignores a release with no acquire",
                 "[application][display][refresh_period]") {
    ScopedTimerPeriods restore;
    ScopedTimerPeriods::set(40, 40);
    RefreshPeriodHold hold;
    hold.set_period(16);

    hold.release();
    CHECK_FALSE(hold.is_held());
    CHECK(default_refr_timer_period() == 40);

    // The unmatched release banked nothing: one acquire holds, one release lets go.
    hold.acquire();
    CHECK(default_refr_timer_period() == 16);
    hold.release();
    CHECK_FALSE(hold.is_held());
    CHECK(default_refr_timer_period() == 40);
}

TEST_CASE_METHOD(LVGLTestFixture, "RefreshPeriodHold with no period leaves the timers alone",
                 "[application][display][refresh_period]") {
    ScopedTimerPeriods restore;
    ScopedTimerPeriods::set(40, 45);
    RefreshPeriodHold hold;

    hold.acquire();
    CHECK(hold.is_held());
    CHECK(default_refr_timer_period() == 40);
    CHECK(anim_timer_period() == 45);
    hold.release();
    CHECK_FALSE(hold.is_held());
    CHECK(default_refr_timer_period() == 40);
    CHECK(anim_timer_period() == 45);
}

TEST_CASE_METHOD(LVGLTestFixture, "RefreshPeriodHold tolerates a missing refresh timer",
                 "[application][display][refresh_period]") {
    ScopedTimerPeriods restore;
    ScopedTimerPeriods::set(40, 45);
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);
    RefreshPeriodHold hold;
    hold.set_period(16);

    SECTION("no default display") {
        lv_display_set_default(nullptr);
        hold.acquire();
        lv_display_set_default(disp);
    }

    SECTION("a default display whose refresh timer is gone") {
        lv_display_t* bare = lv_display_create(32, 32);
        REQUIRE(bare != nullptr);
        lv_display_delete_refr_timer(bare);
        lv_display_set_default(bare);
        hold.acquire();
        lv_display_set_default(disp);
        lv_display_delete(bare);
    }

    CHECK(hold.is_held());
    CHECK(default_refr_timer_period() == 40);
    CHECK(anim_timer_period() == 45);

    hold.release();
    CHECK_FALSE(hold.is_held());
    // Nothing was changed, so nothing is written back.
    CHECK(default_refr_timer_period() == 40);
    CHECK(anim_timer_period() == 45);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "RefreshPeriodHold restores the display it changed, not whichever is default now",
                 "[application][display][refresh_period]") {
    ScopedTimerPeriods restore;
    ScopedTimerPeriods::set(40, 45);
    lv_display_t* disp = lv_display_get_default();
    RefreshPeriodHold hold;
    hold.set_period(16);

    lv_display_t* other = lv_display_create(32, 32);
    REQUIRE(other != nullptr);
    lv_display_set_default(other);
    const uint32_t other_baseline = default_refr_timer_period();
    REQUIRE(other_baseline != 40);

    hold.acquire();
    CHECK(default_refr_timer_period() == 16);
    lv_display_set_default(disp);

    SECTION("still alive at release") {
        hold.release();
        CHECK(lv_display_get_refr_timer(other)->period == other_baseline);
        CHECK(default_refr_timer_period() == 40);
        lv_display_delete(other);
    }

    SECTION("deleted mid-hold") {
        lv_display_delete(other);
        hold.release();
        CHECK(default_refr_timer_period() == 40);
    }

    CHECK_FALSE(hold.is_held());
    CHECK(anim_timer_period() == 45);
}

TEST_CASE("a refresh period hold leaked by one LVGL fixture does not reach the next",
          "[application][display][refresh_period]") {
    RefreshPeriodHold& hold = helix::active_refresh_period_hold();
    uint32_t baseline = 0;
    {
        LVGLTestFixture leaking;
        baseline = default_refr_timer_period();
        REQUIRE(baseline != 16);
        hold.set_period(16);
        hold.acquire();
        REQUIRE(default_refr_timer_period() == 16);
    }

    LVGLTestFixture next;
    const bool held_on_entry = hold.is_held();
    const uint32_t refr_on_entry = default_refr_timer_period();
    const uint32_t anim_on_entry = anim_timer_period();
    const uint32_t configured_on_entry = hold.period();
    // Leave nothing held or configured for the tests after this one, whatever the outcome.
    RefreshPeriodHoldTestAccess::reset(hold);

    CHECK_FALSE(held_on_entry);
    CHECK(refr_on_entry == baseline);
    CHECK(anim_on_entry == baseline);
    CHECK(configured_on_entry == 0);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "apply_refresh_timing sets the display and animation timers and the saver period",
                 "[application][display][refresh_period]") {
    ScopedTimerPeriods restore;
    ScopedEnv period("HELIX_REFR_PERIOD_MS");
    ScopedEnv scope("HELIX_REFR_PERIOD_SCOPE");
    ScopedEnv saver("HELIX_SCREENSAVER_REFR_PERIOD_MS");
    setenv("HELIX_REFR_PERIOD_MS", "20", 1);
    unsetenv("HELIX_REFR_PERIOD_SCOPE");
    setenv("HELIX_SCREENSAVER_REFR_PERIOD_MS", "16", 1);
    ScopedIndev input;
    auto& queue = helix::ui::UpdateQueue::instance();
    const uint32_t read_before = read_timer_period(input.indev);
    const uint32_t queue_before = helix::ui::UpdateQueueTestAccess::timer_period(queue);
    REQUIRE(read_before != 20);
    REQUIRE(queue_before != 20);

    helix::apply_refresh_timing(helix::refresh_timing_from_env());

    CHECK(default_refr_timer_period() == 20);
    CHECK(anim_timer_period() == 20);
    CHECK(helix::active_refresh_period_hold().period() == 16);
    // The default scope leaves input reads and the update queue alone.
    CHECK(read_timer_period(input.indev) == read_before);
    CHECK(helix::ui::UpdateQueueTestAccess::timer_period(queue) == queue_before);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "HELIX_REFR_PERIOD_SCOPE=all also paces every input read and the update queue",
                 "[application][display][refresh_period][update_queue]") {
    ScopedTimerPeriods restore;
    ScopedEnv period("HELIX_REFR_PERIOD_MS");
    ScopedEnv scope("HELIX_REFR_PERIOD_SCOPE");
    setenv("HELIX_REFR_PERIOD_MS", "20", 1);
    setenv("HELIX_REFR_PERIOD_SCOPE", "all", 1);
    ScopedIndev pointer;
    ScopedIndev second;

    helix::apply_refresh_timing(helix::refresh_timing_from_env());

    CHECK(default_refr_timer_period() == 20);
    CHECK(anim_timer_period() == 20);
    CHECK(read_timer_period(pointer.indev) == 20);
    CHECK(read_timer_period(second.indev) == 20);
    CHECK(helix::ui::UpdateQueueTestAccess::timer_period(helix::ui::UpdateQueue::instance()) == 20);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "apply_refresh_timing with nothing set keeps the global timers and arms the "
                 "screensaver period",
                 "[application][display][refresh_period]") {
    ScopedTimerPeriods restore;
    ScopedTimerPeriods::set(40, 45);
    ScopedIndev input;
    const uint32_t read_before = read_timer_period(input.indev);
    helix::active_refresh_period_hold().set_period(33);

    helix::apply_refresh_timing(helix::RefreshTiming{});

    CHECK(default_refr_timer_period() == 40);
    CHECK(anim_timer_period() == 45);
    CHECK(read_timer_period(input.indev) == read_before);
    CHECK(helix::active_refresh_period_hold().period() == 16);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "ScopedTimerPeriods puts back the input read and drain timer periods it found",
                 "[application][display][refresh_period][update_queue]") {
    ScopedIndev lasting;
    std::optional<ScopedIndev> doomed;
    doomed.emplace();
    auto& queue = helix::ui::UpdateQueue::instance();
    const uint32_t refr_before = default_refr_timer_period();
    const uint32_t anim_before = anim_timer_period();
    const uint32_t read_before = read_timer_period(lasting.indev);
    const uint32_t drain_before = helix::ui::UpdateQueueTestAccess::timer_period(queue);
    REQUIRE(read_before != 20);
    REQUIRE(drain_before != 20);

    {
        ScopedTimerPeriods restore;
        helix::RefreshTiming timing;
        timing.refr_period_ms = 20;
        timing.scope_all = true;
        helix::apply_refresh_timing(timing);
        REQUIRE(read_timer_period(lasting.indev) == 20);
        REQUIRE(helix::ui::UpdateQueueTestAccess::timer_period(queue) == 20);
        // A device the guard saw that is deleted before the guard ends is skipped.
        doomed.reset();
    }

    CHECK(default_refr_timer_period() == refr_before);
    CHECK(anim_timer_period() == anim_before);
    CHECK(read_timer_period(lasting.indev) == read_before);
    CHECK(helix::ui::UpdateQueueTestAccess::timer_period(queue) == drain_before);
}

namespace {

/// Gives back the shared hold however the test exits; a release with none out does nothing.
struct ReleaseActiveHold {
    ~ReleaseActiveHold() {
        helix::active_refresh_period_hold().release();
    }
};

} // namespace

TEST_CASE_METHOD(
    LVGLTestFixture,
    "apply_refresh_timing keeps a held screensaver period and restores to the new global one",
    "[application][display][refresh_period]") {
    ScopedTimerPeriods restore;
    ScopedTimerPeriods::set(40, 45);
    RefreshPeriodHold& hold = helix::active_refresh_period_hold();
    ReleaseActiveHold release_on_exit;
    helix::RefreshTiming timing;
    timing.refr_period_ms = 20;
    timing.screensaver_refr_period_ms = 16;

    SECTION("on the display the hold changed") {
        helix::apply_refresh_timing(timing);
        hold.acquire();
        REQUIRE(default_refr_timer_period() == 16);

        helix::apply_refresh_timing(timing);
        CHECK(hold.is_held());
        CHECK(default_refr_timer_period() == 16);
        CHECK(anim_timer_period() == 16);

        hold.release();
        CHECK(default_refr_timer_period() == 20);
        CHECK(anim_timer_period() == 20);
    }

    SECTION("with no global period configured") {
        timing.refr_period_ms = 0;
        helix::apply_refresh_timing(timing);
        hold.acquire();
        REQUIRE(default_refr_timer_period() == 16);

        helix::apply_refresh_timing(timing);
        CHECK(default_refr_timer_period() == 16);
        CHECK(anim_timer_period() == 16);

        hold.release();
        CHECK(default_refr_timer_period() == 40);
        CHECK(anim_timer_period() == 45);
    }

    SECTION("after the display the hold changed was replaced") {
        lv_display_t* disp = lv_display_get_default();
        lv_display_t* replaced = lv_display_create(32, 32);
        REQUIRE(replaced != nullptr);
        lv_display_set_default(replaced);
        helix::apply_refresh_timing(timing);
        hold.acquire();
        const bool fast_on_replaced = lv_display_get_refr_timer(replaced)->period == 16;
        lv_display_set_default(disp);
        lv_display_delete(replaced);
        REQUIRE(fast_on_replaced);

        helix::apply_refresh_timing(timing);
        CHECK(default_refr_timer_period() == 16);
        CHECK(anim_timer_period() == 16);

        hold.release();
        CHECK(default_refr_timer_period() == 20);
        CHECK(anim_timer_period() == 20);
    }
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "the main loop takes the screensaver floor only while the hold runs its timers",
                 "[application][display][refresh_period]") {
    ScopedTimerPeriods restore;
    ScopedTimerPeriods::set(40, 45);
    RefreshPeriodHold& hold = helix::active_refresh_period_hold();
    ReleaseActiveHold release_on_exit;
    helix::RefreshTiming timing; // floor 5, screensaver floor 1, screensaver period 16

    SECTION("with a screensaver period") {
        helix::apply_refresh_timing(timing);
        CHECK(hold.loop_min_sleep_ms(timing.loop_min_sleep_ms) == 5);
        hold.acquire();
        REQUIRE(default_refr_timer_period() == 16);
        CHECK(hold.loop_min_sleep_ms(timing.loop_min_sleep_ms) == 1);
        hold.release();
        CHECK(hold.loop_min_sleep_ms(timing.loop_min_sleep_ms) == 5);
    }

    SECTION("with the screensaver period off") {
        timing.screensaver_refr_period_ms = 0;
        helix::apply_refresh_timing(timing);
        hold.acquire();
        REQUIRE(default_refr_timer_period() == 40);
        CHECK(hold.loop_min_sleep_ms(timing.loop_min_sleep_ms) == 5);
        hold.release();
    }
}

TEST_CASE_METHOD(
    LVGLTestFixture,
    "RefreshPeriodHold without a configured period waits for the saver's and follows it",
    "[application][display][refresh_period]") {
    ScopedTimerPeriods restore;
    ScopedTimerPeriods::set(40, 45);
    RefreshPeriodHold hold;

    hold.follow(33); // not held: changes nothing
    CHECK(default_refr_timer_period() == 40);

    hold.acquire();
    CHECK(default_refr_timer_period() == 40);
    hold.follow(16);
    CHECK(default_refr_timer_period() == 16);
    CHECK(anim_timer_period() == 16);
    hold.follow(33);
    CHECK(default_refr_timer_period() == 33);
    CHECK(anim_timer_period() == 33);

    hold.release();
    CHECK_FALSE(hold.is_held());
    CHECK(default_refr_timer_period() == 40);
    CHECK(anim_timer_period() == 45);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a held RefreshPeriodHold moves from its configured period to the saver's",
                 "[application][display][refresh_period]") {
    ScopedTimerPeriods restore;
    ScopedTimerPeriods::set(40, 45);
    RefreshPeriodHold hold;
    hold.set_period(20);

    hold.acquire();
    CHECK(default_refr_timer_period() == 20);
    hold.follow(20); // a saver at level 0, which takes the configured period
    CHECK(default_refr_timer_period() == 20);
    hold.follow(33); // the same saver at level 1
    CHECK(default_refr_timer_period() == 33);
    CHECK(anim_timer_period() == 33);

    hold.release();
    CHECK(default_refr_timer_period() == 40);
    CHECK(anim_timer_period() == 45);

    // The saver's period ends with the hold: the next acquire starts at the configured one.
    hold.acquire();
    CHECK(default_refr_timer_period() == 20);
    hold.release();
    CHECK(default_refr_timer_period() == 40);
}
