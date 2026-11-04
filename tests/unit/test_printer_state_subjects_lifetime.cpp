// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// PrinterState::get_subjects_lifetime() — the death signal every observer on a
// PrinterState-owned subject must carry.
//
// The bug these pin: PrintStatusPanel is a process-lifetime singleton
// (get_global_print_status_panel()) that observes ~25 PrinterState subjects. It
// therefore outlives PrinterState::deinit_subjects() — printer switching in
// production, per-fixture teardown in tests. deinit_subjects() runs
// lv_subject_deinit(), which frees every observer node on those subjects. An
// ObserverGuard that was never given a lifetime token cannot know that, so its
// next reset() calls lv_observer_remove() on freed memory and dereferences
// observer->subject: SIGSEGV at lv_observer.c:584.
//
// Both directions matter and both are asserted below. Skipping the removal when
// the subject is NOT dead is its own use-after-free — it orphans a live observer
// node whose context is about to be freed (bundles 449TVQ82 / X3RA4252) — so an
// empty token is never an acceptable stand-in for a live one.

#include "ui_observer_guard.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "observer_factory.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

namespace {

struct CountingPanel {
    int notifications = 0;
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "PrinterState subjects lifetime is never an empty token",
                 "[printer_state][observer][crash_hardening]") {
    PrinterState state;

    // Before init_subjects(): an empty token would read as "subject already
    // dead" in ObserverGuard::reset() and suppress every removal.
    SubjectLifetime before_init = state.get_subjects_lifetime();
    REQUIRE(before_init != nullptr);
    REQUIRE(*before_init == true);

    state.init_subjects(false);
    SubjectLifetime during = state.get_subjects_lifetime();
    REQUIRE(during != nullptr);
    REQUIRE(*during == true);

    // init_subjects() must NOT mint a replacement — an observer that subscribed
    // before this call holds `before_init`, and that token has to be the one
    // deinit_subjects() flips.
    REQUIRE(during == before_init);

    state.deinit_subjects();

    // The token handed out earlier now reports death...
    REQUIRE(*during == false);
    // ...and the accessor still hands out a LIVE one for the next cycle rather
    // than a null (which would silently disable removal for everything after).
    SubjectLifetime after = state.get_subjects_lifetime();
    REQUIRE(after != nullptr);
    REQUIRE(*after == true);
    REQUIRE(after != during);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "ObserverGuard on a PrinterState subject survives deinit_subjects()",
                 "[printer_state][observer][crash_hardening]") {
    PrinterState state;
    state.init_subjects(false);

    CountingPanel panel;
    ObserverGuard guard = helix::ui::observe<int>(
        state.print_state().get_print_progress_subject(), &panel,
        [](CountingPanel* p, int /*v*/) { p->notifications++; }, state.get_subjects_lifetime());
    REQUIRE(static_cast<bool>(guard));

    // Teardown frees every observer node on that subject.
    state.deinit_subjects();

    // reset() must detect the death and SKIP lv_observer_remove(). Without the
    // token this dereferences observer->subject on freed memory — a segfault
    // under a normal build, a heap-use-after-free under ASAN.
    REQUIRE_NOTHROW(guard.reset());
    REQUIRE_FALSE(static_cast<bool>(guard));
}

// The other direction: the token must not become a blanket "never remove".
// While the subjects are alive, reset() has to unlink the observer, or the node
// is orphaned on a live subject and the next notify runs a freed context.
TEST_CASE_METHOD(LVGLTestFixture,
                 "ObserverGuard still removes normally while PrinterState subjects are alive",
                 "[printer_state][observer][crash_hardening]") {
    PrinterState state;
    state.init_subjects(false);

    lv_subject_t* subject = state.print_state().get_print_progress_subject();
    REQUIRE(subject != nullptr);
    const uint32_t baseline = lv_ll_get_len(&subject->subs_ll);

    CountingPanel panel;
    {
        ObserverGuard guard = helix::ui::observe<int>(
            subject, &panel, [](CountingPanel* p, int /*v*/) { p->notifications++; },
            state.get_subjects_lifetime());
        REQUIRE(lv_ll_get_len(&subject->subs_ll) == baseline + 1);

        guard.reset();
        // Skipping here would leave the count at baseline + 1.
        REQUIRE(lv_ll_get_len(&subject->subs_ll) == baseline);
    }

    // And the detached observer must not be notified any more.
    panel.notifications = 0;
    lv_subject_set_int(subject, 42);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(panel.notifications == 0);

    state.deinit_subjects();
}
