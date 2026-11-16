// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_rotation_probe_tap_detection.cpp
 * @brief Tests for the two defects that made the first-boot rotation probe
 *        register zero taps on a working touchscreen (K1C)
 *
 * 1. The probe's poll loop calls lv_timer_handler() every iteration. The
 *    debounced resize fanout runs the theme layout refresh from there, which
 *    measured 2.9s of every 5s window on that device - the loop took no touch
 *    sample for most of the window a user was reacting in. DisplayManager now
 *    suspends the fanout for the duration of the probe.
 *
 * 2. LVGL's evdev read drains the whole fd per call and reports only the final
 *    state, so a press and its release arriving between two polls collapse into
 *    one RELEASED sample. Level-testing `state == PRESSED` drops that tap
 *    silently. helix::TapLatch latches the edge instead.
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/display_manager_test_access.h"
#include "display_manager.h"
#include "tap_latch.h"

#include "../catch_amalgamated.hpp"

using helix::TapLatch;

// ============================================================================
// Helpers
// ============================================================================

namespace {

lv_indev_data_t sample(lv_indev_state_t state, int32_t x, int32_t y) {
    lv_indev_data_t d = {};
    d.state = state;
    d.point.x = x;
    d.point.y = y;
    return d;
}

lv_indev_data_t released_at(int32_t x, int32_t y) {
    return sample(LV_INDEV_STATE_RELEASED, x, y);
}

lv_indev_data_t pressed_at(int32_t x, int32_t y) {
    return sample(LV_INDEV_STATE_PRESSED, x, y);
}

// ResizeCallback is a plain function pointer, so the observed count has to be
// file scope. Reset it at the start of every test that uses it.
int g_resize_calls = 0;

void count_resize() {
    g_resize_calls++;
}

/// Timers whose user data points anywhere inside @p owner, members included.
int timers_pointing_into(const DisplayManager& owner) {
    const auto* lo = reinterpret_cast<const char*>(&owner);
    const auto* hi = lo + sizeof(DisplayManager);
    int n = 0;
    for (lv_timer_t* t = lv_timer_get_next(nullptr); t != nullptr; t = lv_timer_get_next(t)) {
        const auto* ud = static_cast<const char*>(lv_timer_get_user_data(t));
        n += (ud >= lo && ud < hi) ? 1 : 0;
    }
    return n;
}

/// Event callbacks on @p obj whose user data is @p owner.
int event_cbs_pointing_at(lv_obj_t* obj, const void* owner) {
    int n = 0;
    for (uint32_t i = 0; i < lv_obj_get_event_count(obj); i++) {
        n += lv_event_dsc_get_user_data(lv_obj_get_event_dsc(obj, i)) == owner ? 1 : 0;
    }
    return n;
}

// Callbacks for the register-during-fan-out test. ResizeCallback is a plain
// function pointer, so each distinct callback needs its own function.
DisplayManager* g_registering_mgr = nullptr;
int g_registering_calls = 0;
int g_late_calls = 0;

void cb_a() {
    g_resize_calls++;
}
void cb_b() {
    g_resize_calls++;
}
void cb_c() {
    g_resize_calls++;
}
void late_1() {
    g_late_calls++;
}
void late_2() {
    g_late_calls++;
}
void late_3() {
    g_late_calls++;
}
void late_4() {
    g_late_calls++;
}
void late_5() {
    g_late_calls++;
}
void late_6() {
    g_late_calls++;
}
void late_7() {
    g_late_calls++;
}
void late_8() {
    g_late_calls++;
}

void register_late_callbacks() {
    g_registering_calls++;
    for (auto cb : {late_1, late_2, late_3, late_4, late_5, late_6, late_7, late_8}) {
        g_registering_mgr->register_resize_callback(cb);
    }
}

// A resize callback that raises one more SIZE_CHANGED on g_reentrant_target from
// inside the fan-out, the way a layout refresh resizing the screen does.
lv_obj_t* g_reentrant_target = nullptr;
int g_reentrant_calls = 0;

void resize_and_resize_again() {
    if (g_reentrant_calls++ == 0) {
        lv_obj_send_event(g_reentrant_target, LV_EVENT_SIZE_CHANGED, nullptr);
    }
}

} // namespace

/**
 * @brief A settled object with the resize handler installed on it
 *
 * The object's own layout is flushed before the handler is attached, so the
 * only LV_EVENT_SIZE_CHANGED the test observes are the ones it sends.
 */
class ResizeFixture : public LVGLTestFixture {
  protected:
    lv_obj_t* attach_resize_handler(DisplayManager& mgr) {
        lv_obj_t* obj = lv_obj_create(test_screen());
        lv_obj_set_size(obj, 200, 100);
        process_lvgl(300); // flush layout so no spontaneous SIZE_CHANGED follows
        mgr.init_resize_handler(obj);
        mgr.register_resize_callback(count_resize);
        g_resize_calls = 0;
        return obj;
    }
};

// ============================================================================
// TapLatch - edge detection
// ============================================================================

TEST_CASE("TapLatch latches an observed press and consume() clears it",
          "[display][rotation_probe][tap_latch]") {
    TapLatch latch;

    latch.feed(released_at(0, 0));
    REQUIRE_FALSE(latch.latched());

    latch.feed(pressed_at(120, 90));
    REQUIRE(latch.latched());

    REQUIRE(latch.consume());
    REQUIRE_FALSE(latch.latched());
    REQUIRE_FALSE(latch.consume());
}

TEST_CASE("TapLatch recovers a press+release collapsed into a single read",
          "[display][rotation_probe][tap_latch]") {
    // The actual field bug: the poll loop is blocked inside lv_timer_handler()
    // for the whole tap, evdev drains press *and* release in the next read, and
    // the only sample the loop ever sees is RELEASED at the tap position.
    TapLatch latch;

    latch.feed(released_at(0, 0)); // idle baseline before the tap
    REQUIRE_FALSE(latch.latched());

    latch.feed(released_at(240, 160)); // press+release, collapsed
    REQUIRE(latch.latched());
    REQUIRE(latch.from_collapsed_read());
    REQUIRE(latch.consume());
}

TEST_CASE("TapLatch does not latch on an idle pointer", "[display][rotation_probe][tap_latch]") {
    TapLatch latch;

    for (int i = 0; i < 50; i++) {
        latch.feed(released_at(400, 240)); // evdev repeats the last position
    }

    REQUIRE_FALSE(latch.latched());
    REQUIRE_FALSE(latch.consume());
}

TEST_CASE("TapLatch first sample after reset only re-baselines",
          "[display][rotation_probe][tap_latch]") {
    // Between probe screens the latch is reset. The position left behind by the
    // previous screen's tap must not read as a fresh tap on the next one.
    TapLatch latch;

    latch.feed(released_at(0, 0));
    latch.feed(released_at(240, 160));
    REQUIRE(latch.consume());

    latch.reset();
    latch.feed(released_at(240, 160)); // same stale position, first sample
    REQUIRE_FALSE(latch.latched());

    latch.feed(released_at(241, 161)); // a new contact moves it again
    REQUIRE(latch.latched());
}

TEST_CASE("TapLatch with collapsed detection off ignores pointer motion",
          "[display][rotation_probe][tap_latch]") {
    // SDL/mouse: motion is reported with no button down, so the coordinate rule
    // would latch on every wiggle. Press detection still works.
    TapLatch latch(false);

    latch.feed(released_at(0, 0));
    latch.feed(released_at(100, 100));
    latch.feed(released_at(200, 200));
    REQUIRE_FALSE(latch.latched());

    latch.feed(pressed_at(200, 200));
    REQUIRE(latch.latched());
    REQUIRE_FALSE(latch.from_collapsed_read());
}

TEST_CASE("TapLatch survives a tap whose press is visible and release follows",
          "[display][rotation_probe][tap_latch]") {
    // Normal, unblocked case: press seen on one poll, release on the next.
    TapLatch latch;

    latch.feed(released_at(10, 10));
    latch.feed(pressed_at(300, 200));
    REQUIRE(latch.consume());

    latch.feed(released_at(300, 200));
    // The release of a tap already consumed must not re-latch on its own
    // position (it did not move).
    REQUIRE_FALSE(latch.latched());
}

// ============================================================================
// Resize fanout suspension - the probe's poll loop must not block
// ============================================================================

TEST_CASE_METHOD(ResizeFixture, "Resize fanout runs when not suspended",
                 "[display][rotation_probe][resize]") {
    DisplayManager mgr;
    lv_obj_t* screen = attach_resize_handler(mgr);

    REQUIRE_FALSE(mgr.resize_fanout_suspended());

    lv_obj_send_event(screen, LV_EVENT_SIZE_CHANGED, nullptr);
    process_lvgl(400); // debounce is 250ms

    REQUIRE(g_resize_calls == 1);

    lv_obj_delete(screen); // drop the handler before mgr goes out of scope
}

TEST_CASE_METHOD(ResizeFixture, "Suspended resize fanout arms no debounce timer",
                 "[display][rotation_probe][resize]") {
    DisplayManager mgr;
    lv_obj_t* screen = attach_resize_handler(mgr);

    mgr.set_resize_fanout_suspended(true);
    REQUIRE(mgr.resize_fanout_suspended());

    // Four rotations, as the probe cycles them.
    for (int i = 0; i < 4; i++) {
        lv_obj_send_event(screen, LV_EVENT_SIZE_CHANGED, nullptr);
        process_lvgl(400);
    }

    REQUIRE(g_resize_calls == 0);

    // Resuming restores normal behaviour for later resizes.
    mgr.set_resize_fanout_suspended(false);
    lv_obj_send_event(screen, LV_EVENT_SIZE_CHANGED, nullptr);
    process_lvgl(400);
    REQUIRE(g_resize_calls == 1);

    lv_obj_delete(screen);
}

TEST_CASE_METHOD(ResizeFixture, "A debounce armed before suspension does not fan out",
                 "[display][rotation_probe][resize]") {
    // The probe can start with a resize already pending from startup. That
    // timer must expire without running the callbacks, or the first
    // lv_timer_handler() of the first probe screen pays for the theme refresh.
    DisplayManager mgr;
    lv_obj_t* screen = attach_resize_handler(mgr);

    lv_obj_send_event(screen, LV_EVENT_SIZE_CHANGED, nullptr);
    process_lvgl(50); // timer armed, not yet expired

    mgr.set_resize_fanout_suspended(true);
    process_lvgl(400); // timer expires while suspended

    REQUIRE(g_resize_calls == 0);

    // The dropped timer must not linger: a later resize after resuming still
    // fires exactly once.
    mgr.set_resize_fanout_suspended(false);
    lv_obj_send_event(screen, LV_EVENT_SIZE_CHANGED, nullptr);
    process_lvgl(400);
    REQUIRE(g_resize_calls == 1);

    lv_obj_delete(screen);
}

TEST_CASE_METHOD(ResizeFixture, "a destroyed DisplayManager leaves no resize debounce armed",
                 "[display][rotation_probe][resize]") {
    // The debounce's callback dereferences its DisplayManager, so a timer that survives
    // the manager fires into freed memory, in whichever test is running then.
    lv_obj_t* screen = nullptr;
    {
        DisplayManager mgr;
        screen = attach_resize_handler(mgr);
        lv_obj_send_event(screen, LV_EVENT_SIZE_CHANGED, nullptr);
        process_lvgl(50); // timer armed, not yet expired
        REQUIRE(timers_pointing_into(mgr) == 1);
    }
    process_lvgl(400); // past the debounce window
    CHECK(g_resize_calls == 0);

    lv_obj_delete(screen);
}

TEST_CASE_METHOD(ResizeFixture, "a destroyed DisplayManager is detached from its screen",
                 "[display][rotation_probe][resize]") {
    // The screen outlives the manager here, so a SIZE_CHANGED after destruction must
    // not reach the freed manager through the event callback's user data.
    const void* mgr_addr = nullptr;
    lv_obj_t* screen = nullptr;
    {
        DisplayManager mgr;
        mgr_addr = &mgr;
        screen = attach_resize_handler(mgr);
        REQUIRE(event_cbs_pointing_at(screen, mgr_addr) == 2); // SIZE_CHANGED + DELETE
    }
    CHECK(event_cbs_pointing_at(screen, mgr_addr) == 0);

    lv_obj_send_event(screen, LV_EVENT_SIZE_CHANGED, nullptr);
    process_lvgl(400);
    CHECK(g_resize_calls == 0);

    lv_obj_delete(screen);
}

TEST_CASE_METHOD(ResizeFixture, "shutdown detaches the resize handler and cancels the debounce",
                 "[display][rotation_probe][resize]") {
    DisplayManager mgr;
    lv_obj_t* screen = attach_resize_handler(mgr);
    lv_obj_send_event(screen, LV_EVENT_SIZE_CHANGED, nullptr);
    process_lvgl(50);
    REQUIRE(timers_pointing_into(mgr) == 1);

    mgr.shutdown();

    CHECK(event_cbs_pointing_at(screen, &mgr) == 0);
    CHECK(timers_pointing_into(mgr) == 0);
    process_lvgl(400);
    CHECK(g_resize_calls == 0);

    lv_obj_delete(screen);
}

TEST_CASE_METHOD(ResizeFixture, "shutdown after the screen is deleted is safe",
                 "[display][rotation_probe][resize]") {
    DisplayManager mgr;
    lv_obj_t* screen = attach_resize_handler(mgr);
    REQUIRE(DisplayManagerTestAccess::resize_screen(mgr) == screen);

    lv_obj_delete(screen);
    CHECK(DisplayManagerTestAccess::resize_screen(mgr) == nullptr);

    mgr.shutdown();
}

TEST_CASE_METHOD(ResizeFixture, "A resize callback may register another during the fan-out",
                 "[display][rotation_probe][resize]") {
    DisplayManager mgr;
    lv_obj_t* screen = attach_resize_handler(mgr);
    g_registering_mgr = &mgr;
    g_registering_calls = 0;
    g_late_calls = 0;
    // The registering callback sits mid-vector and adds enough that the vector
    // reallocates, so the walk still has entries to read after it returns.
    for (auto cb : {cb_a, register_late_callbacks, cb_b, cb_c}) {
        mgr.register_resize_callback(cb);
    }

    lv_obj_send_event(screen, LV_EVENT_SIZE_CHANGED, nullptr);
    process_lvgl(400);
    CHECK(g_registering_calls == 1);
    CHECK(g_resize_calls == 4); // count_resize + cb_a..cb_c
    CHECK(g_late_calls == 0);   // registered mid-fan-out: runs from the next resize

    lv_obj_send_event(screen, LV_EVENT_SIZE_CHANGED, nullptr);
    process_lvgl(400);
    CHECK(g_registering_calls == 2);
    CHECK(g_late_calls == 8);

    g_registering_mgr = nullptr;
    lv_obj_delete(screen);
}

TEST_CASE_METHOD(ResizeFixture,
                 "Resize debounce is trailing-edge: a resize inside the window "
                 "pushes the fan-out back",
                 "[display][rotation_probe][resize]") {
    DisplayManager mgr;
    lv_obj_t* screen = attach_resize_handler(mgr);

    lv_obj_send_event(screen, LV_EVENT_SIZE_CHANGED, nullptr);
    process_lvgl(150);
    lv_obj_send_event(screen, LV_EVENT_SIZE_CHANGED, nullptr);
    process_lvgl(150); // 300ms after the first resize, 150ms after the second
    CHECK(g_resize_calls == 0);

    process_lvgl(250);
    CHECK(g_resize_calls == 1);

    process_lvgl(400);
    CHECK(g_resize_calls == 1);

    lv_obj_delete(screen);
}

TEST_CASE_METHOD(ResizeFixture, "A resize raised during the fan-out arms a fresh debounce",
                 "[display][rotation_probe][resize]") {
    DisplayManager mgr;
    lv_obj_t* screen = attach_resize_handler(mgr);
    mgr.register_resize_callback(resize_and_resize_again);
    g_reentrant_target = screen;
    g_reentrant_calls = 0;

    lv_obj_send_event(screen, LV_EVENT_SIZE_CHANGED, nullptr);
    process_lvgl(400);
    REQUIRE(g_reentrant_calls == 1);
    REQUIRE(timers_pointing_into(mgr) == 1);

    SECTION("the fresh debounce fans out") {
        process_lvgl(400);
        CHECK(g_reentrant_calls == 2);
        CHECK(g_resize_calls == 2);
    }

    SECTION("the fresh debounce is owned, so shutdown cancels it") {
        mgr.shutdown();
        CHECK(timers_pointing_into(mgr) == 0);
        process_lvgl(400);
        CHECK(g_reentrant_calls == 1);
    }

    g_reentrant_target = nullptr;
    lv_obj_delete(screen);
}
