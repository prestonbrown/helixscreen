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
        ObserverGuard guard = helix::ui::observe_int_sync<CountingPanel>(
            &subject, &panel, [](CountingPanel* p, int /*v*/) { p->notifications++; });
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

    ObserverGuard::revalidate_all();
    lv_subject_deinit(&subject);
}
