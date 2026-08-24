// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_estop_observer_lifetime.cpp
 * @brief EmergencyStopOverlay's observers must survive a PrinterState deinit cycle.
 *
 * EmergencyStopOverlay is a process-lifetime singleton, but the PrinterState it
 * observes is not: test fixtures own one per case, and production tears the whole
 * tree down and rebuilds it on a soft restart (Add Printer). That is exactly the
 * case PrinterState::get_subjects_lifetime() exists for — see THREADING.md §5 and
 * the accessor's own doc comment.
 *
 * Without that token, create()'s three ObserverGuards never learn that
 * lv_subject_deinit() already freed their observer nodes, so the next reset()
 * calls lv_observer_remove() on freed memory. The guard's epoch fallback does not
 * cover it: ObserverGuard::invalidate_all() runs only in production teardown
 * (application.cpp), never in the fixtures.
 */

#include "ui_emergency_stop.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/emergency_stop_test_access.h"
#include "moonraker_api.h"
#include "printer_state.h"

#include <lvgl.h>

#include "../catch_amalgamated.hpp"

namespace {

using Access = EmergencyStopOverlayTestAccess;

/// Canary proving the rebuilt subject's observer list is still intact after the
/// estop guards reset. Its node is the only one legitimately on that list.
int g_sentinel_fires = 0;

void sentinel_cb(lv_observer_t* /*observer*/, lv_subject_t* /*subject*/) {
    ++g_sentinel_fires;
}

/// Push a value the subject is not already holding — lv_subject_set_int()
/// short-circuits on an unchanged value and would notify nobody.
void poke(lv_subject_t* subject) {
    lv_subject_set_int(subject, lv_subject_get_int(subject) == 0 ? 1 : 0);
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "EmergencyStopOverlay observers survive a PrinterState deinit cycle",
                 "[recovery][threading][lifetime]") {
    auto& estop = EmergencyStopOverlay::instance();

    // Exactly how Application::init_panel_subjects() wires it (application.cpp).
    // The fixture already ran init_subjects().
    estop.init(state(), api());
    estop.create();

    // One of the three subjects create() subscribed to. Its storage is a member
    // of a PrinterState component, so the address stays valid across the cycle
    // below — only the observer nodes hanging off it are freed.
    lv_subject_t* klippy = state().get_klippy_state_subject();
    REQUIRE(klippy != nullptr);

    // Teardown + rebuild, the soft-restart shape. deinit_subjects() flips
    // subjects_lifetime_ to false and lv_subject_deinit()s everything, which
    // frees every observer node registered by create().
    state().deinit_subjects();
    state().init_subjects(true);

    // A live observer on the rebuilt subject. If the estop guards below unlink
    // their freed nodes from this list, they corrupt it and take the sentinel
    // with them.
    g_sentinel_fires = 0;
    lv_observer_t* sentinel = lv_subject_add_observer(klippy, sentinel_cb, nullptr);
    REQUIRE(sentinel != nullptr);
    REQUIRE(g_sentinel_fires == 1); // observers fire once on subscribe

    // The reset that turns the missing token into a use-after-free.
    estop.deinit_subjects();

    // The rebuilt subject must still be intact and still notifying.
    poke(klippy);
    CHECK(g_sentinel_fires == 2);
    poke(klippy);
    CHECK(g_sentinel_fires == 3);

    lv_observer_remove(sentinel);
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "EmergencyStopOverlay re-create rebinds to the rebuilt subjects",
                 "[recovery][threading][lifetime]") {
    auto& estop = EmergencyStopOverlay::instance();
    Access::reset_recovery_reason(estop);
    Access::reset_suppression(estop); // don't inherit a window from an earlier test

    estop.init(state(), api());
    estop.create();

    state().deinit_subjects();
    state().init_subjects(true);

    // create() is documented as re-runnable, and a soft restart takes this path.
    // Each guard's move-assignment reset()s the stale one first, so this is the
    // second site that touches the freed observer nodes.
    estop.init(state(), api());
    estop.create();

    // create() reset the deliberate first-fire skip, so burn it with a state the
    // recovery path ignores, then a real SHUTDOWN must reach the dialog through
    // the freshly bound observer.
    state().set_klippy_state_sync(KlippyState::READY);
    process_lvgl(50);
    REQUIRE(Access::recovery_reason(estop) == RecoveryReason::NONE);

    state().set_klippy_state_sync(KlippyState::SHUTDOWN);
    process_lvgl(50);
    CHECK(Access::recovery_reason(estop) == RecoveryReason::SHUTDOWN);
    CHECK(lv_obj_find_by_name(lv_screen_active(), "klipper_recovery_card") != nullptr);

    estop.deinit_subjects();
    helix::ui::UpdateQueue::instance().drain();
}
