// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_observer_guard_non_registry_subject.cpp
 * @brief ObserverGuard must not skip lv_observer_remove() for subjects
 *        StaticSubjectRegistry never frees.
 *
 * invalidate_all() marks "the registry's subjects were just freed by
 * deinit_all()". Subjects the registry does NOT free — the file-static theme
 * globals (ui_breakpoint, ui_breakpoint_v, ui_is_portrait) are the live
 * examples — are reseeded in place, never freed, and their owners declare
 * that with ObserverGuard::mark_subject_teardown_exempt(). An epoch bump
 * elsewhere in the process must not suppress removal of an observer on such
 * a subject. A suppressed removal frees the observer context while the
 * observer stays on the live subject; the next lv_subject_notify() then
 * reads the freed context.
 */

#include "ui_observer_guard.h"
#include "ui_update_queue.h"

#include "../helix_test_fixture.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/update_queue_test_access.h"
#include "observer_factory.h"

#include "../catch_amalgamated.hpp"

using namespace helix::ui;

namespace {

struct CountingPanel {
    int notifications = 0;
};

void drain() {
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
}

} // namespace

TEST_CASE_METHOD(
    LVGLTestFixture,
    "ObserverGuard removes observer from a teardown-exempt subject after invalidate_all",
    "[observer][raii][crash_hardening]") {
    // Standalone subject the registry never deinits — the shape of the theme
    // globals, reseeded in place, declared exempt by their owner.
    lv_subject_t subject;
    lv_subject_init_int(&subject, 0);
    ObserverGuard::mark_subject_teardown_exempt(&subject);
    REQUIRE(lv_ll_get_len(&subject.subs_ll) == 0);

    CountingPanel panel;
    {
        ObserverGuard guard = helix::ui::observe<int>(
            &subject, &panel, [](CountingPanel* p, int /*v*/) { p->notifications++; },
            subject_never_freed());
        REQUIRE(lv_ll_get_len(&subject.subs_ll) == 1);

        // A printer-state teardown elsewhere in the process bumps the epoch.
        // The registry's subjects were freed; THIS subject was not.
        ObserverGuard::invalidate_all();

        guard.reset();

        // The observer must be gone from the live subject. Skipping removal
        // here orphans it with a freed context.
        REQUIRE(lv_ll_get_len(&subject.subs_ll) == 0);
    }

    // A later notify must not reach the freed context.
    lv_subject_set_int(&subject, 99);
    drain();
    CHECK(panel.notifications == 0);

    lv_subject_deinit(&subject);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "ObserverGuard with an owner's token removes its observer from a surviving "
                 "subject after invalidate_all",
                 "[observer][raii][crash_hardening]") {
    lv_subject_t subject;
    lv_subject_init_int(&subject, 0);
    SubjectLifetime owner_token = std::make_shared<bool>(true);

    CountingPanel panel;
    {
        ObserverGuard guard = helix::ui::observe<int>(
            &subject, &panel, [](CountingPanel* p, int /*v*/) { p->notifications++; }, owner_token);
        REQUIRE(lv_ll_get_len(&subject.subs_ll) == 1);

        ObserverGuard::invalidate_all();
        guard.reset();
        REQUIRE(lv_ll_get_len(&subject.subs_ll) == 0);
    }

    lv_subject_set_int(&subject, 99);
    drain();
    CHECK(panel.notifications == 0);
    lv_subject_deinit(&subject);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "ObserverGuard with an owner's token still skips removal once the owner flips it",
                 "[observer][raii][crash_hardening]") {
    lv_subject_t subject;
    lv_subject_init_int(&subject, 0);
    SubjectLifetime owner_token = std::make_shared<bool>(true);

    CountingPanel panel;
    ObserverGuard guard = helix::ui::observe<int>(
        &subject, &panel, [](CountingPanel* p, int /*v*/) { p->notifications++; }, owner_token);
    ObserverGuard::invalidate_all();
    *owner_token = false;
    lv_subject_deinit(&subject);
    guard.reset();
    // The subject was freed, so its list is empty and reset() left it alone.
    CHECK(lv_ll_get_len(&subject.subs_ll) == 0);
    CHECK_FALSE(static_cast<bool>(guard));
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "ObserverGuard without a token keeps skipping removal after invalidate_all",
                 "[observer][raii][crash_hardening]") {
    lv_subject_t subject;
    lv_subject_init_int(&subject, 0);

    CountingPanel panel;
    ObserverGuard guard = helix::ui::observe<int>(
        &subject, &panel, [](CountingPanel* p, int /*v*/) { p->notifications++; },
        SubjectLifetime{});
    ObserverGuard::invalidate_all();
    lv_subject_deinit(&subject);
    guard.reset();
    CHECK(lv_ll_get_len(&subject.subs_ll) == 0);
    CHECK_FALSE(static_cast<bool>(guard));
}

// An observer created after invalidate_all() sits on a live subject, so resetting it
// must remove it; skipping removal orphans the node and a later notify reaches the
// freed context.
TEST_CASE_METHOD(
    LVGLTestFixture,
    "ObserverGuard removes an observer created after invalidate_all from a live subject",
    "[observer][raii][crash_hardening]") {
    lv_subject_t subject;
    lv_subject_init_int(&subject, 0);
    REQUIRE(lv_ll_get_len(&subject.subs_ll) == 0);

    CountingPanel panel;
    ObserverGuard::invalidate_all();

    ObserverGuard guard = helix::ui::observe<int>(
        &subject, &panel, [](CountingPanel* p, int /*v*/) { p->notifications++; },
        subject_never_freed());
    REQUIRE(lv_ll_get_len(&subject.subs_ll) == 1);

    guard.reset();
    REQUIRE(lv_ll_get_len(&subject.subs_ll) == 0);

    panel.notifications = 0;
    lv_subject_set_int(&subject, 99);
    drain();
    REQUIRE(panel.notifications == 0);

    lv_subject_deinit(&subject);
}

// lv_subject_add_observer_obj() returns null for a subject still typed
// LV_SUBJECT_TYPE_INVALID (observed before its owner's init_subjects() ran). The guard
// still owns the context the factory allocated and must free it.
TEST_CASE_METHOD(LVGLTestFixture, "ObserverGuard frees its context when the attach failed",
                 "[observer][lifetime][1279]") {
    lv_subject_t never_initialized{};
    int ctx = 0;
    bool context_freed = false;

    {
        ObserverGuard guard(
            &never_initialized, [](lv_observer_t*, lv_subject_t*) {}, &ctx,
            [&context_freed]() { context_freed = true; });
        REQUIRE_FALSE(static_cast<bool>(guard));
        CHECK_FALSE(context_freed);
    }

    CHECK(context_freed);
}

// The epoch is process-global: a test that simulates teardown must not leave it bumped, or
// every long-lived guard created earlier skips removal from its live subject when it dies.
TEST_CASE("HelixTestFixture rolls back an invalidate_all() made inside a test",
          "[observer][raii][crash_hardening]") {
    const uint64_t before = ObserverGuard::invalidation_epoch();
    {
        HelixTestFixture fixture;
        ObserverGuard::invalidate_all();
        REQUIRE(ObserverGuard::invalidation_epoch() == before + 1);
    }
    CHECK(ObserverGuard::invalidation_epoch() == before);
}
