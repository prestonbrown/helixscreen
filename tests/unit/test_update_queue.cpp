// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../test_helpers/widget_in_tree.h"
#include "lvgl_test_fixture.h"
#include "observer_factory.h"
#include "test_helpers/log_capture.h"
#include "test_helpers/update_queue_test_access.h"

#include <memory>
#include <system_error>

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
// observe<int> + ScopedFreeze interaction
//
// observe<int> defers its initial callback via queue_update(). Under the
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

// The demangled name of a std type depends on the standard library's inline
// namespace: libc++ names it std::__1::X, libstdc++ names it std::X.
bool logs_std_type(const helix::TextLogCapture& capture, const std::string& name) {
    return capture.contains("std::" + name) || capture.contains("std::__1::" + name);
}
} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "observe<int> initial callback is buffered during ScopedFreeze",
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
        guard = helix::ui::observe<int>(
            &subject, &panel, [](FakePanel* p, int value) { p->observed_value = value; },
            subject_never_freed());

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

TEST_CASE_METHOD(LVGLTestFixture, "observe<int> initial callback works without ScopedFreeze",
                 "[update_queue][observer]") {
    auto& q = UpdateQueue::instance();
    FakePanel panel;

    lv_subject_t subject;
    lv_subject_init_int(&subject, 42);

    {
        // Create observer without freeze — initial fire should be delivered.
        auto guard = helix::ui::observe<int>(
            &subject, &panel, [](FakePanel* p, int value) { p->observed_value = value; },
            subject_never_freed());

        UpdateQueueTestAccess::drain(q);
        REQUIRE(panel.observed_value == 42);
    }

    lv_subject_deinit(&subject);
}

TEST_CASE_METHOD(LVGLTestFixture, "observe<int> subsequent changes are delivered after thaw",
                 "[update_queue][observer]") {
    auto& q = UpdateQueue::instance();
    FakePanel panel;

    lv_subject_t subject;
    lv_subject_init_int(&subject, 0);

    ObserverGuard guard;
    {
        auto freeze = q.scoped_freeze();

        // Initial fire buffered during freeze.
        guard = helix::ui::observe<int>(
            &subject, &panel, [](FakePanel* p, int value) { p->observed_value = value; },
            subject_never_freed());
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
// Draining once per refresh period is as often as the UI can use.
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

// A callback that throws must not take the drain batch with it, and the log
// must identify the producer and the exception well enough to act on: the
// tag, the type name, and for std::system_error the numeric code both ways —
// what()'s strerror text alone can render a pointer-shaped code as a bare
// "Unknown error -1288124992" with no hint of which callback threw it.
TEST_CASE_METHOD(LVGLTestFixture,
                 "a throwing system_error callback is contained and fully identified",
                 "[update_queue]") {
    auto& q = UpdateQueue::instance();
    const auto before = UpdateQueueTestAccess::callback_exception_count();
    bool ran_after_throw = false;

    helix::TextLogCapture capture;

    q.queue("ThrowingProducer::boom", []() {
        // A pointer-shaped error value: no real errno is anywhere near this
        // large. Decimal plus hex in the log is what makes that visible.
        throw std::system_error(static_cast<int>(0xB338C5C0u), std::generic_category());
    });
    q.queue("AfterThrow::still_runs", [&ran_after_throw]() { ran_after_throw = true; });
    UpdateQueueTestAccess::drain_all(q);

    CHECK(UpdateQueueTestAccess::callback_exception_count() == before + 1);
    CHECK(ran_after_throw);

    const auto logged = capture.get_captured();
    CHECK(capture.contains("[ThrowingProducer::boom]"));
    CHECK(logs_std_type(capture, "system_error"));
    CHECK(capture.contains("-1288124992"));
    CHECK(capture.contains("b338c5c0"));
    CHECK(capture.contains("Unknown error -1288124992"));
}

TEST_CASE_METHOD(LVGLTestFixture, "an untagged throwing callback is named by its enqueue site",
                 "[update_queue]") {
    auto& q = UpdateQueue::instance();
    const auto before = UpdateQueueTestAccess::callback_exception_count();

    helix::TextLogCapture capture;

    q.queue([]() { throw std::runtime_error("boom"); });
    UpdateQueueTestAccess::drain_all(q);

    CHECK(UpdateQueueTestAccess::callback_exception_count() == before + 1);
    CHECK(capture.contains("std::runtime_error"));
    CHECK(capture.contains("boom"));
    // The untagged enqueue recorded this file as its call site.
    CHECK(capture.contains("test_update_queue.cpp"));
}

TEST_CASE_METHOD(LVGLTestFixture, "a non-std exception names its type with no message",
                 "[update_queue]") {
    auto& q = UpdateQueue::instance();
    const auto before = UpdateQueueTestAccess::callback_exception_count();

    helix::TextLogCapture capture;

    q.queue("IntThrower::raw", []() { throw 42; });
    UpdateQueueTestAccess::drain_all(q);

    CHECK(UpdateQueueTestAccess::callback_exception_count() == before + 1);
    CHECK(capture.contains("IntThrower::raw"));
    CHECK(capture.contains("int"));
    CHECK(capture.contains("(no message)"));
}

// The unique_ptr forms own their payload even when the queue drops the
// callback unrun (shutdown, a test fixture discarding the queue).
TEST_CASE_METHOD(LVGLTestFixture, "queue_update frees its payload when the callback is dropped",
                 "[update_queue]") {
    auto alive = std::make_shared<int>(0);
    std::weak_ptr<int> probe = alive;
    auto widget = lv_obj_create(lv_screen_active());

    helix::ui::queue_update<std::shared_ptr<int>>(std::make_unique<std::shared_ptr<int>>(alive),
                                                  [](std::shared_ptr<int>*) {});
    helix::ui::queue_update(widget, std::make_unique<std::shared_ptr<int>>(alive),
                            [](lv_obj_t*, std::shared_ptr<int>*) {});
    alive.reset();
    REQUIRE_FALSE(probe.expired());

    (void)helix::ui::UpdateQueueTestAccess::discard_pending(helix::ui::UpdateQueue::instance());
    REQUIRE(probe.expired());
    lv_obj_delete(widget);
}

TEST_CASE_METHOD(LVGLTestFixture, "is_draining holds through nested drains and ends with them",
                 "[update_queue][lifecycle]") {
    auto& q = UpdateQueue::instance();
    REQUIRE_FALSE(q.is_draining());

    bool in_outer = false;
    bool in_inner = false;
    bool after_inner = false;
    q.queue("outer", [&]() {
        in_outer = q.is_draining();
        q.queue("inner", [&]() { in_inner = q.is_draining(); });
        q.drain();
        after_inner = q.is_draining();
    });
    q.drain();

    CHECK(in_outer);
    CHECK(in_inner);
    CHECK(after_inner);
    CHECK_FALSE(q.is_draining());

    // A callback that throws still ends its drain.
    q.queue("throws", []() { throw std::runtime_error("boom"); });
    q.drain();
    CHECK_FALSE(q.is_draining());
}

TEST_CASE_METHOD(LVGLTestFixture, "Widget-safe queue_update: valid widget executes callback",
                 "[widget_safe][async]") {
    lv_obj_t* widget = lv_obj_create(test_screen());
    REQUIRE(widget != nullptr);

    bool callback_fired = false;
    auto data = std::make_unique<int>(42);

    helix::ui::queue_update<int>(widget, std::move(data), [&callback_fired](lv_obj_t* w, int* d) {
        REQUIRE(helix::test::widget_in_tree(w));
        REQUIRE(*d == 42);
        callback_fired = true;
    });

    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    REQUIRE(callback_fired);
}

TEST_CASE_METHOD(LVGLTestFixture, "Widget-safe queue_update: deleted widget skips callback",
                 "[widget_safe][async]") {
    lv_obj_t* widget = lv_obj_create(test_screen());
    REQUIRE(widget != nullptr);

    bool callback_fired = false;
    auto data = std::make_unique<int>(99);

    helix::ui::queue_update<int>(widget, std::move(data),
                                 [&callback_fired](lv_obj_t*, int*) { callback_fired = true; });

    // Destroy widget before draining queue
    lv_obj_delete(widget);

    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    REQUIRE_FALSE(callback_fired);
}

TEST_CASE_METHOD(LVGLTestFixture, "Widget-safe queue_update: data freed even when skipped",
                 "[widget_safe][async]") {
    lv_obj_t* widget = lv_obj_create(test_screen());
    REQUIRE(widget != nullptr);

    struct TrackDestruction {
        bool* destroyed;
        ~TrackDestruction() {
            *destroyed = true;
        }
    };

    bool data_destroyed = false;
    auto data = std::make_unique<TrackDestruction>();
    data->destroyed = &data_destroyed;

    helix::ui::queue_update<TrackDestruction>(widget, std::move(data),
                                              [](lv_obj_t*, TrackDestruction*) {});

    // Destroy widget — callback should be skipped but data still freed
    lv_obj_delete(widget);

    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    REQUIRE(data_destroyed);
}

TEST_CASE_METHOD(LVGLTestFixture, "queue_widget_update: valid widget executes callback",
                 "[widget_safe][async]") {
    lv_obj_t* widget = lv_obj_create(test_screen());
    REQUIRE(widget != nullptr);

    bool callback_fired = false;

    helix::ui::queue_widget_update(widget, [&callback_fired](lv_obj_t* w) {
        REQUIRE(helix::test::widget_in_tree(w));
        callback_fired = true;
    });

    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    REQUIRE(callback_fired);
}

TEST_CASE_METHOD(LVGLTestFixture, "queue_widget_update: deleted widget skips callback",
                 "[widget_safe][async]") {
    lv_obj_t* widget = lv_obj_create(test_screen());
    REQUIRE(widget != nullptr);

    bool callback_fired = false;

    helix::ui::queue_widget_update(widget, [&callback_fired](lv_obj_t*) { callback_fired = true; });

    lv_obj_delete(widget);

    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    REQUIRE_FALSE(callback_fired);
}
