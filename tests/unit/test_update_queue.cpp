// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lvgl_test_fixture.h"
#include "observer_factory.h"
#include "test_helpers/update_queue_test_access.h"

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;
using helix::ui::UpdateQueueTestAccess;

TEST_CASE_METHOD(LVGLTestFixture, "drain works before freeze", "[update_queue]") {
    auto& q = UpdateQueue::instance();
    bool first_ran = false;

    // Queue and drain before freezing — callback should run
    q.queue([&first_ran]() { first_ran = true; });
    UpdateQueueTestAccess::drain(q);
    REQUIRE(first_ran);
}

TEST_CASE_METHOD(LVGLTestFixture, "drain inside freeze is a no-op for buffered work",
                 "[update_queue]") {
    auto& q = UpdateQueue::instance();
    bool ran = false;

    {
        auto freeze = q.scoped_freeze();
        q.queue([&ran]() { ran = true; });
        // Drain inside freeze: the work was diverted into frozen_buffer_,
        // so pending_ is empty and the drain is a no-op.
        UpdateQueueTestAccess::drain(q);
        REQUIRE_FALSE(ran);
    }
    // Freeze released → buffer spliced into pending_. Flush before captures
    // go out of scope.
    UpdateQueueTestAccess::drain(q);
    REQUIRE(ran);
}

TEST_CASE_METHOD(LVGLTestFixture, "ScopedFreeze buffers, splices on release", "[update_queue]") {
    auto& q = UpdateQueue::instance();
    bool a = false;
    bool b = false;

    {
        auto freeze = q.scoped_freeze();
        q.queue("a-buffered", [&a]() { a = true; });
        q.queue("b-buffered", [&b]() { b = true; });
        // Drain inside freeze is a no-op for buffered work.
        UpdateQueueTestAccess::drain(q);
        REQUIRE_FALSE(a);
        REQUIRE_FALSE(b);
    }
    // Freeze released → buffer spliced into pending_. Drain fires both.
    UpdateQueueTestAccess::drain(q);
    REQUIRE(a);
    REQUIRE(b);
}

TEST_CASE_METHOD(LVGLTestFixture, "ScopedFreeze is RAII — thaw on scope exit", "[update_queue]") {
    auto& q = UpdateQueue::instance();
    bool ran = false;

    // Freeze in inner scope
    { auto freeze = q.scoped_freeze(); }

    // After scope exit, queue should work again
    q.queue([&ran]() { ran = true; });
    UpdateQueueTestAccess::drain(q);

    REQUIRE(ran);
}

TEST_CASE_METHOD(LVGLTestFixture, "queue resumes after thaw — buffered + post-thaw both run",
                 "[update_queue]") {
    auto& q = UpdateQueue::instance();
    bool buffered_ran = false;
    bool resumed_ran = false;

    // Queue inside freeze — goes into frozen_buffer_, spliced into pending_
    // when the freeze releases at scope exit.
    {
        auto freeze = q.scoped_freeze();
        q.queue([&buffered_ran]() { buffered_ran = true; });
    }

    // After thaw, queue another callback. Drain fires both: the spliced-in
    // buffered one and the post-thaw one.
    q.queue([&resumed_ran]() { resumed_ran = true; });
    UpdateQueueTestAccess::drain(q);

    REQUIRE(buffered_ran);
    REQUIRE(resumed_ran);
}

// ---------------------------------------------------------------------------
// observe_int_sync + ScopedFreeze interaction
//
// observe_int_sync defers its initial callback via queue_update(). Under the
// buffer-not-drop semantics, an observer created during a freeze has its
// initial fire diverted into frozen_buffer_ and spliced into pending_ when
// the freeze releases, so it fires on the next drain — no manual rebind
// needed. Pre-2026-05-11 the initial fire was silently dropped (the root
// cause of the "carousel fans show 0%" bug); this is preserved as a
// regression test for the buffer-and-splice behavior.
// ---------------------------------------------------------------------------

namespace {
struct FakePanel {
    int observed_value = -1;
};
} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "observe_int_sync initial callback is buffered during ScopedFreeze",
                 "[update_queue][observer]") {
    auto& q = UpdateQueue::instance();
    FakePanel panel;

    lv_subject_t subject;
    lv_subject_init_int(&subject, 42);

    // Guard must outlive the freeze. In real code the guard is a member of
    // the widget that registered it; destroying it before the buffered initial
    // fire is drained expires its alive shared_ptr, and the deferred body's
    // weak_alive check skips the handler.
    ObserverGuard guard;
    {
        auto freeze = q.scoped_freeze();

        // Create observer while frozen — the initial fire is queued via
        // queue_update(), goes into frozen_buffer_.
        guard = helix::ui::observe_int_sync<FakePanel>(
            &subject, &panel, [](FakePanel* p, int value) { p->observed_value = value; });

        // Drain inside freeze is a no-op — buffer has not been spliced yet.
        UpdateQueueTestAccess::drain(q);
        REQUIRE(panel.observed_value == -1);
    }

    // After thaw, the buffered initial fire is in pending_. Drain delivers it.
    UpdateQueueTestAccess::drain(q);
    REQUIRE(panel.observed_value == 42);

    guard.reset();
    lv_subject_deinit(&subject);
}

TEST_CASE_METHOD(LVGLTestFixture, "observe_int_sync initial callback works without ScopedFreeze",
                 "[update_queue][observer]") {
    auto& q = UpdateQueue::instance();
    FakePanel panel;

    lv_subject_t subject;
    lv_subject_init_int(&subject, 42);

    {
        // Create observer without freeze — initial fire should be delivered.
        auto guard = helix::ui::observe_int_sync<FakePanel>(
            &subject, &panel, [](FakePanel* p, int value) { p->observed_value = value; });

        UpdateQueueTestAccess::drain(q);
        REQUIRE(panel.observed_value == 42);
    }

    lv_subject_deinit(&subject);
}

TEST_CASE_METHOD(LVGLTestFixture, "observe_int_sync subsequent changes are delivered after thaw",
                 "[update_queue][observer]") {
    auto& q = UpdateQueue::instance();
    FakePanel panel;

    lv_subject_t subject;
    lv_subject_init_int(&subject, 0);

    ObserverGuard guard;
    {
        auto freeze = q.scoped_freeze();

        // Initial fire buffered during freeze.
        guard = helix::ui::observe_int_sync<FakePanel>(
            &subject, &panel, [](FakePanel* p, int value) { p->observed_value = value; });
    }

    // Buffered initial fire spliced into pending_ on freeze release — drain
    // delivers it.
    UpdateQueueTestAccess::drain(q);
    REQUIRE(panel.observed_value == 0);

    // A subsequent subject change is also delivered.
    lv_subject_set_int(&subject, 99);
    UpdateQueueTestAccess::drain(q);
    REQUIRE(panel.observed_value == 99);

    guard.reset();
    lv_subject_deinit(&subject);
}

TEST_CASE_METHOD(LVGLTestFixture, "tagged queue_update sets current_callback_tag during execution",
                 "[update_queue]") {
    auto& q = UpdateQueue::instance();
    const char* tag_during_callback = nullptr;

    q.queue("TestTag::my_callback", [&tag_during_callback]() {
        tag_during_callback = UpdateQueue::current_callback_tag();
    });
    UpdateQueueTestAccess::drain(q);

    REQUIRE(tag_during_callback != nullptr);
    REQUIRE(std::string(tag_during_callback) == "TestTag::my_callback");
    // Tag should be cleared after callback completes
    REQUIRE(UpdateQueue::current_callback_tag() == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "untagged queue_update has null callback tag", "[update_queue]") {
    auto& q = UpdateQueue::instance();
    const char* tag_during_callback = reinterpret_cast<const char*>(0x1); // sentinel

    q.queue(
        [&tag_during_callback]() { tag_during_callback = UpdateQueue::current_callback_tag(); });
    UpdateQueueTestAccess::drain(q);

    REQUIRE(tag_during_callback == nullptr);
}

// The drain timer is what decides how often the whole main loop wakes up:
// lv_timer_handler() returns the shortest time until any live timer is due, and
// the main loop sleeps for that answer. A sub-frame period therefore pins the
// loop at its 5 ms floor — ~200 wakeups a second on a machine with nothing to
// do — and each one pays clock reads, a notification-queue lock, and a drain.
// Draining once per rendered frame is as often as the UI can use.
TEST_CASE_METHOD(LVGLTestFixture, "drain timer does not wake the main loop faster than a frame",
                 "[update_queue][1440]") {
    auto& q = UpdateQueue::instance();
    REQUIRE(q.timer() != nullptr);

    INFO("drain timer period = " << UpdateQueueTestAccess::timer_period(q) << " ms");
    CHECK(UpdateQueueTestAccess::timer_period(q) >= LV_DEF_REFR_PERIOD);
}

// The early-out that skips building a local queue when nothing is pending must
// not swallow work: a drain that finds the queue empty has to leave frozen
// callbacks buffered, and the next drain has to run everything that arrived in
// between — including callbacks enqueued from inside another callback.
TEST_CASE_METHOD(LVGLTestFixture, "empty drain leaves buffered and later work intact",
                 "[update_queue][1440]") {
    auto& q = UpdateQueue::instance();
    bool frozen_ran = false;
    bool nested_ran = false;

    {
        auto freeze = q.scoped_freeze();
        q.queue("frozen", [&frozen_ran]() { frozen_ran = true; });
        // pending_ is empty here — the work went to the frozen buffer.
        UpdateQueueTestAccess::drain(q);
        CHECK_FALSE(frozen_ran);
    }

    q.queue("nests",
            [&q, &nested_ran]() { q.queue("nested", [&nested_ran]() { nested_ran = true; }); });
    UpdateQueueTestAccess::drain_all(q);

    CHECK(frozen_ran);
    CHECK(nested_ran);
}
