// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_observer_factory.cpp
 * @brief Unit tests for observer factory helpers
 *
 * Tests the observe_* factory functions for correctness,
 * edge cases, and behavior preservation.
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/update_queue_test_access.h"
#include "observer_factory.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::ui;
using helix::PrintJobState;

// Helper to drain the update queue after subject changes.
// observe<int> and observe<const char*> defer callbacks via ui_queue_update.
static void drain() {
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
}

// ============================================================================
// Test Panel Class - Mimics a real panel for testing
// ============================================================================

struct TestPanel {
    int int_value = 0;
};

// ============================================================================
// Deferred safety test — observer reassignment during notification
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "Factory: observe<int> safe under observer reassignment",
                 "[factory][observer][safety]") {
    lv_subject_t subject_a;
    lv_subject_t subject_b;
    lv_subject_init_int(&subject_a, 0);
    lv_subject_init_int(&subject_b, 100);

    TestPanel panel;
    ObserverGuard inner_guard;

    // Outer observer reassigns inner_guard when notified — this is the
    // exact pattern that caused the crash in issue #82.
    inner_guard = observe<int>(
        &subject_b, &panel, [](TestPanel* p, int value) { p->int_value = value; },
        subject_never_freed());
    drain();

    auto outer_guard = observe<int>(
        &subject_a, &panel,
        [&inner_guard, &subject_b](TestPanel* p, int /*value*/) {
            // Reassign inner observer — old one is destroyed here.
            // With deferred execution, this is safe.
            inner_guard = observe<int>(
                &subject_b, p, [](TestPanel* pp, int v) { pp->int_value = v * 2; },
                subject_never_freed());
        },
        subject_never_freed());
    drain();

    // Trigger the outer observer — should safely reassign inner
    lv_subject_set_int(&subject_a, 1);
    drain(); // outer fires, reassigns inner_guard
    drain(); // inner's initial callback fires

    // Now inner should use the new handler (value * 2)
    lv_subject_set_int(&subject_b, 50);
    drain();
    REQUIRE(panel.int_value == 100); // 50 * 2

    outer_guard.reset();
    inner_guard.reset();
    lv_subject_deinit(&subject_a);
    lv_subject_deinit(&subject_b);
}

// ============================================================================
// Dispatch and teardown contract, pinned for every family
// ============================================================================

namespace {
bool queue_empty() {
    return UpdateQueueTestAccess::queue_empty(helix::ui::UpdateQueue::instance());
}

struct Recorder {
    std::vector<int> ints;
    std::vector<std::string> strings;
};
} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "Factory: a guard reset before drain drops the queued call",
                 "[factory][observer][safety]") {
    static char buf[16] = "";
    lv_subject_t int_subject;
    lv_subject_t str_subject;
    lv_subject_init_int(&int_subject, 0);
    lv_subject_init_string(&str_subject, buf, nullptr, sizeof(buf), "");
    Recorder rec;
    ObserverGuard guard;

    SECTION("observe<int>") {
        guard = observe<int>(
            &int_subject, &rec, [](Recorder* r, int v) { r->ints.push_back(v); },
            subject_never_freed());
    }
    SECTION("observe<const char*>") {
        guard = observe<const char*>(
            &str_subject, &rec, [](Recorder* r, const char* s) { r->strings.push_back(s); },
            subject_never_freed());
    }
    SECTION("observe_print_state") {
        guard = observe_print_state<Recorder>(
            &int_subject, &rec,
            [](Recorder* r, PrintJobState s) { r->ints.push_back(static_cast<int>(s)); },
            subject_never_freed());
    }
    SECTION("observe_print_lifecycle") {
        guard = observe_print_lifecycle<Recorder>(
            &int_subject, &rec,
            [](Recorder* r, PrintState s) { r->ints.push_back(static_cast<int>(s)); },
            subject_never_freed());
    }

    REQUIRE(guard);
    drain();
    rec = Recorder{};

    lv_subject_set_int(&int_subject, 2);
    lv_subject_copy_string(&str_subject, "x");
    guard.reset();
    drain();

    CHECK(rec.ints.empty());
    CHECK(rec.strings.empty());

    lv_subject_deinit(&int_subject);
    lv_subject_deinit(&str_subject);
}

TEST_CASE_METHOD(LVGLTestFixture, "Factory: deferred families deliver the value at notify time",
                 "[factory][observer]") {
    static char buf[16] = "";
    lv_subject_t int_subject;
    lv_subject_t str_subject;
    lv_subject_init_int(&int_subject, 0);
    lv_subject_init_string(&str_subject, buf, nullptr, sizeof(buf), "");
    Recorder rec;

    auto g1 = observe<int>(
        &int_subject, &rec, [](Recorder* r, int v) { r->ints.push_back(v); },
        subject_never_freed());
    auto g2 = observe<const char*>(
        &str_subject, &rec, [](Recorder* r, const char* s) { r->strings.push_back(s); },
        subject_never_freed());

    // Subscription queues one call carrying the current value; nothing runs before drain.
    CHECK(rec.ints.empty());
    CHECK(rec.strings.empty());
    CHECK_FALSE(queue_empty());
    drain();
    CHECK(rec.ints == std::vector<int>{0});
    CHECK(rec.strings == std::vector<std::string>{""});

    // Two notifications before one drain: each call sees its own value, in order, and the
    // string is copied at notify time, not read back from the subject's buffer at drain.
    lv_subject_set_int(&int_subject, 1);
    lv_subject_set_int(&int_subject, 2);
    lv_subject_copy_string(&str_subject, "a");
    lv_subject_copy_string(&str_subject, "b");
    CHECK(rec.ints.size() == 1);
    drain();
    CHECK(rec.ints == std::vector<int>{0, 1, 2});
    CHECK(rec.strings == std::vector<std::string>{"", "a", "b"});

    g1.reset();
    g2.reset();
    lv_subject_deinit(&int_subject);
    lv_subject_deinit(&str_subject);
}

TEST_CASE_METHOD(LVGLTestFixture, "Factory: immediate families run inside the notification",
                 "[factory][observer]") {
    static char buf[16] = "";
    lv_subject_t int_subject;
    lv_subject_t str_subject;
    lv_subject_init_int(&int_subject, 0);
    lv_subject_init_string(&str_subject, buf, nullptr, sizeof(buf), "");
    drain();
    Recorder rec;

    auto g1 = observe<int>(
        &int_subject, &rec, [](Recorder* r, int v) { r->ints.push_back(v); }, subject_never_freed(),
        Dispatch::Immediate);
    auto g2 = observe<const char*>(
        &str_subject, &rec, [](Recorder* r, const char* s) { r->strings.push_back(s); },
        subject_never_freed(), Dispatch::Immediate);
    auto g3 = observe_print_state(
        &int_subject, &rec,
        [](Recorder* r, PrintJobState s) { r->ints.push_back(100 + static_cast<int>(s)); },
        subject_never_freed(), Dispatch::Immediate);

    // Subscription fires once, synchronously, and queues nothing.
    CHECK(rec.ints == std::vector<int>{0, 100});
    CHECK(rec.strings == std::vector<std::string>{""});
    CHECK(queue_empty());

    lv_subject_set_int(&int_subject, static_cast<int>(PrintJobState::PAUSED));
    lv_subject_copy_string(&str_subject, "now");
    CHECK(rec.ints == std::vector<int>{0, 100, 2, 102});
    CHECK(rec.strings == std::vector<std::string>{"", "now"});
    CHECK(queue_empty());

    g1.reset();
    g2.reset();
    g3.reset();
    lv_subject_deinit(&int_subject);
    lv_subject_deinit(&str_subject);
}

TEST_CASE_METHOD(LVGLTestFixture, "Factory: handler captures are freed after reset and drain",
                 "[factory][observer][raii]") {
    lv_subject_t subject;
    lv_subject_init_int(&subject, 0);
    Recorder rec;
    auto state = std::make_shared<int>(0);
    std::weak_ptr<int> weak_state = state;
    ObserverGuard guard;

    SECTION("deferred") {
        guard = observe<int>(
            &subject, &rec, [state](Recorder*, int v) { *state = v; }, subject_never_freed());
    }
    SECTION("immediate") {
        guard = observe<int>(
            &subject, &rec, [state](Recorder*, int v) { *state = v; }, subject_never_freed(),
            Dispatch::Immediate);
    }
    state.reset();
    REQUIRE_FALSE(weak_state.expired());

    // A notification may leave a queued call holding the handler; resetting the guard
    // while it is pending must still free everything once the queue drains.
    lv_subject_set_int(&subject, 3);
    guard.reset();
    drain();
    CHECK(weak_state.expired());

    lv_subject_deinit(&subject);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "Factory: a dead subject lifetime drops the queued call and frees the context",
                 "[factory][observer][subject_lifetime]") {
    lv_subject_t subject;
    lv_subject_init_int(&subject, 0);
    SubjectLifetime lifetime = std::make_shared<bool>(true);
    Recorder rec;
    auto state = std::make_shared<int>(0);
    std::weak_ptr<int> weak_state = state;

    auto guard = observe<int>(
        &subject, &rec,
        [state](Recorder* r, int v) {
            r->ints.push_back(v);
            *state = v;
        },
        lifetime);
    state.reset();
    drain();
    rec = Recorder{};

    lv_subject_set_int(&subject, 9);
    // Owner teardown order: signal death, then deinit frees the observer node.
    *lifetime = false;
    lv_subject_deinit(&subject);
    guard.reset(); // must not touch the freed observer (ASAN)
    drain();

    CHECK(rec.ints.empty());
    CHECK(weak_state.expired());
}

TEST_CASE_METHOD(LVGLTestFixture, "Factory: null subject or owner returns an empty guard",
                 "[factory][observer][edge]") {
    lv_subject_t subject;
    lv_subject_init_int(&subject, 1);
    int calls = 0;
    auto h_int = [&calls](Recorder*, int) { calls++; };
    auto h_str = [&calls](Recorder*, const char*) { calls++; };
    Recorder rec;

    CHECK_FALSE(observe<int>(nullptr, &rec, h_int, subject_never_freed()));
    CHECK_FALSE(
        observe<int>(&subject, static_cast<Recorder*>(nullptr), h_int, subject_never_freed()));
    CHECK_FALSE(observe<int>(nullptr, &rec, h_int, subject_never_freed(), Dispatch::Immediate));
    CHECK_FALSE(observe<int>(&subject, static_cast<Recorder*>(nullptr), h_int,
                             subject_never_freed(), Dispatch::Immediate));
    CHECK_FALSE(observe<const char*>(nullptr, &rec, h_str, subject_never_freed()));
    CHECK_FALSE(
        observe<const char*>(nullptr, &rec, h_str, subject_never_freed(), Dispatch::Immediate));
    CHECK_FALSE(observe_print_state<Recorder>(
        nullptr, &rec, [](Recorder*, PrintJobState) {}, subject_never_freed()));
    drain();
    CHECK(calls == 0);

    lv_subject_deinit(&subject);
}

namespace {
// Sets *flag when the handler holding it is destroyed. Moves hand the flag over,
// so only the last live copy reports.
struct DestroyProbe {
    bool* flag;
    explicit DestroyProbe(bool* f) : flag(f) {}
    DestroyProbe(const DestroyProbe& o) : flag(o.flag) {}
    DestroyProbe(DestroyProbe&& o) noexcept : flag(std::exchange(o.flag, nullptr)) {}
    ~DestroyProbe() {
        if (flag)
            *flag = true;
    }
};
} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "Factory: an immediate handler that resets its own guard outlives the call",
                 "[factory][observer][safety]") {
    lv_subject_t subject;
    lv_subject_init_int(&subject, 0);
    Recorder rec;
    static bool handler_destroyed = false;
    static bool destroyed_mid_call = true;
    static ObserverGuard* self_guard = nullptr;
    ObserverGuard guard;
    self_guard = &guard;

    guard = observe<int>(
        &subject, &rec,
        [probe = DestroyProbe(&handler_destroyed)](Recorder* r, int v) {
            r->ints.push_back(v);
            if (v == 1) {
                self_guard->reset();
                destroyed_mid_call = handler_destroyed;
            }
        },
        subject_never_freed(), Dispatch::Immediate);
    handler_destroyed = false;

    lv_subject_set_int(&subject, 1);
    CHECK_FALSE(destroyed_mid_call);
    CHECK(handler_destroyed);
    CHECK_FALSE(guard);

    lv_subject_set_int(&subject, 2);
    CHECK(rec.ints == std::vector<int>{0, 1});

    self_guard = nullptr;
    lv_subject_deinit(&subject);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "Factory: a mutable immediate handler keeps its state across notifications",
                 "[factory][observer]") {
    lv_subject_t subject;
    lv_subject_init_int(&subject, 0);
    Recorder rec;

    auto guard = observe<int>(
        &subject, &rec, [n = 0](Recorder* r, int) mutable { r->ints.push_back(++n); },
        subject_never_freed(), Dispatch::Immediate);
    lv_subject_set_int(&subject, 1);
    lv_subject_set_int(&subject, 2);
    CHECK(rec.ints == std::vector<int>{1, 2, 3});

    guard.reset();
    lv_subject_deinit(&subject);
}

// ============================================================================
// SubjectLifetime with several holders: ObserverGuard detects a dead subject by
// the bool value, not by shared_ptr expiry (#816).
//
// When a dynamic subject is destroyed, the source sets *lifetime = false then
// resets its shared_ptr. Other services may still hold copies (refcount > 0).
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "SubjectLifetime: guard skips remove when bool is false but refcount > 0",
                 "[observer_cleanup][crash_hardening][subject_lifetime]") {
    // Simulate: source creates a dynamic subject with lifetime token
    lv_subject_t subject;
    lv_subject_init_int(&subject, 0);
    SubjectLifetime source_lifetime = std::make_shared<bool>(true);

    // Two services observe the same subject and hold lifetime copies
    SubjectLifetime service_a_lifetime = source_lifetime;
    SubjectLifetime service_b_lifetime = source_lifetime;

    // Service A creates an observer (like NozzleTempsWidget)
    struct DummyPanel {
        int count = 0;
    } panel;
    ObserverGuard guard_a = observe<int>(
        &subject, &panel, [](DummyPanel* self, int) { self->count++; }, service_a_lifetime);
    drain();

    // Verify observer works
    panel.count = 0;
    lv_subject_set_int(&subject, 42);
    drain();
    REQUIRE(panel.count == 1);

    // Source signals death and destroys the subject (like init_extruders)
    *source_lifetime = false;
    source_lifetime.reset();
    lv_subject_deinit(&subject); // Frees observer struct!

    // Service A tries to clean up its observer (like clear_rows)
    // The weak_ptr is NOT expired because service_b still holds a copy.
    // But the bool was set to false, so the guard should skip lv_observer_remove.
    REQUIRE_FALSE(service_b_lifetime.use_count() == 0); // Still alive
    service_a_lifetime.reset();
    guard_a.reset(); // Must NOT crash — observer struct was freed by deinit
}

TEST_CASE_METHOD(LVGLTestFixture, "SubjectLifetime: guard still removes when subject is alive",
                 "[observer_cleanup][crash_hardening][subject_lifetime]") {
    // When the subject IS alive (bool = true), guard must call lv_observer_remove
    lv_subject_t subject;
    lv_subject_init_int(&subject, 0);
    SubjectLifetime lifetime = std::make_shared<bool>(true);

    SubjectLifetime observer_lifetime = lifetime;

    struct DummyPanel {
        int count = 0;
    } panel;
    ObserverGuard guard = observe<int>(
        &subject, &panel, [](DummyPanel* self, int) { self->count++; }, observer_lifetime);
    drain();

    // Subject is alive — guard should properly remove the observer
    observer_lifetime.reset();
    guard.reset(); // Should call lv_observer_remove (subject alive, bool = true)

    // After removal, setting subject should not fire any callbacks
    panel.count = 0;
    lv_subject_set_int(&subject, 99);
    drain();
    REQUIRE(panel.count == 0);

    lifetime.reset();
    lv_subject_deinit(&subject);
}

// A moved-from ObserverGuard must not run the cleanup its destination now owns.
//
// std::function's moved-from state is valid but UNSPECIFIED. libc++ keeps the
// target when the callable fits its small-object buffer, and the observer
// factories' `[ctx]() { delete ctx; }` does - one captured pointer. A source
// that kept the cleanup frees the observer's context from its own destructor
// while the destination keeps the observer attached to a live subject, so the
// next lv_subject_notify() reads freed memory inside the synchronous observer
// body. libstdc++ empties the source and hides it (prestonbrown/helixscreen#1446).
TEST_CASE_METHOD(LVGLTestFixture, "ObserverGuard: a moved-from guard runs no cleanup",
                 "[observer_cleanup][crash_hardening][1446]") {
    lv_subject_t subject;
    lv_subject_init_int(&subject, 0);
    auto noop = [](lv_observer_t*, lv_subject_t*) {};
    int cleanups = 0;

    SECTION("move construction") {
        std::optional<ObserverGuard> dst;
        {
            ObserverGuard src(&subject, noop, &cleanups, [&cleanups]() { cleanups++; });
            dst.emplace(std::move(src));
        }
        // src is gone; the context it was constructed with belongs to dst now.
        CHECK(cleanups == 0);
        dst.reset();
        CHECK(cleanups == 1);
    }

    SECTION("move assignment") {
        ObserverGuard dst;
        {
            ObserverGuard src(&subject, noop, &cleanups, [&cleanups]() { cleanups++; });
            dst = std::move(src);
        }
        CHECK(cleanups == 0);
        dst.reset();
        CHECK(cleanups == 1);
    }

    lv_subject_deinit(&subject);
}

// The shape the crash actually took: a factory returns its guard by value, so
// the context outlives every temporary along the way and is still valid when
// the subject notifies.
TEST_CASE_METHOD(LVGLTestFixture,
                 "ObserverGuard: a factory-returned guard survives its temporaries",
                 "[observer_cleanup][crash_hardening][1446]") {
    lv_subject_t subject;
    lv_subject_init_int(&subject, 0);

    struct DummyPanel {
        int count = 0;
    } panel;

    ObserverGuard guard = observe<int>(
        &subject, &panel, [](DummyPanel* self, int) { self->count++; }, subject_never_freed());
    drain();

    panel.count = 0;
    lv_subject_set_int(&subject, 7);
    drain();
    CHECK(panel.count == 1);

    guard.reset();
    lv_subject_deinit(&subject);
}
