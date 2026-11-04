// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_controls_secondary_fans.cpp
 * @brief Design-conformance regression test for ControlsPanel secondary-fan
 *        SubjectLifetime handling (lesson L084 / L077).
 *
 * ControlsPanel::subscribe_to_secondary_fan_speeds() observes the per-fan speed
 * subjects returned by PrinterState::get_fan_speed_subject(name, lifetime). Those
 * subjects are DYNAMIC — freed and recreated whenever fans are re-discovered. The
 * mandatory doctrine is that each observer's SubjectLifetime token must live in a
 * *member* vector (secondary_fan_lifetimes_) that outlives the paired ObserverGuard
 * (secondary_fan_observers_), aligned index-for-index. A stack-local token would
 * expire the guard's weak_ptr immediately, leaving a dangling observer that
 * corrupts the subject's observer list on later teardown.
 *
 * This test locks that in:
 *  - The panel retains a live shared_ptr COPY of each fan's lifetime token in a
 *    member vector aligned with its observers (only a member — not a stack local —
 *    can do this). This test does not even *compile* against the old stack-local
 *    design, because there is no member vector to inspect.
 *  - When the fans are re-discovered and the generic fans are orphaned, each
 *    retained token flips to false (subject death signalled), which the panel can
 *    only observe because it holds a live copy.
 *  - The real teardown/rebuild path (populate_secondary_fans) runs without crashing
 *    even though it resets ObserverGuards whose subjects were already freed —
 *    ObserverGuard::reset() must skip lv_observer_remove() on the dead subjects.
 */

#include "ui_panel_controls.h"

#include "../test_fixtures.h"
#include "../test_helpers/controls_panel_test_access.h"
#include "core/update_queue.h"
#include "printer_state.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::PrinterState;
using helix::ui::ControlsPanelTestAccess;

TEST_CASE_METHOD(XMLTestFixture,
                 "ControlsPanel keeps secondary-fan lifetime tokens as aligned members",
                 "[controls][fans][lifetime]") {
    PrinterState& st = state();

    // Step 1: 3 GENERIC secondary fans (A/B/C) plus a bare PART_COOLING "fan"
    // (excluded from the secondary list). init_subjects() was already called by
    // the XMLTestFixture constructor.
    st.fan_state().init_fans({"fan", "fan_generic A", "fan_generic B", "fan_generic C"});

    // Step 2: trivial ctor — no setup()/XML.
    ControlsPanel panel(st, nullptr);

    // Step 3: seed tracked rows with the 3 generic fans, then subscribe.
    ControlsPanelTestAccess::set_rows(panel, {"fan_generic A", "fan_generic B", "fan_generic C"});
    ControlsPanelTestAccess::subscribe(panel);

    auto& lifetimes = ControlsPanelTestAccess::lifetimes(panel);
    auto& observers = ControlsPanelTestAccess::observers(panel);

    // Step 4: the member vectors exist, are aligned, and hold live tokens.
    REQUIRE(lifetimes.size() == 3);
    REQUIRE(lifetimes.size() == observers.size());
    for (const auto& tok : lifetimes) {
        REQUIRE(tok);          // non-null shared_ptr copy retained by the panel
        REQUIRE(*tok == true); // dynamic subject is alive
    }

    // Step 5: force fan rediscovery that ORPHANS the 3 generic fans. In
    // PrinterFanState::init_fans, orphaned fans get *lifetime = false and their
    // subject is lv_subject_deinit'd.
    st.fan_state().init_fans({"fan"});

    // Step 6: LOAD-BEARING assertion. Each retained member token now reads false.
    // This only passes because the panel holds a live shared copy of the token in
    // a member vector — a stack-local token would have been destroyed at the end
    // of subscribe(), leaving nothing to inspect.
    for (const auto& tok : lifetimes) {
        REQUIRE(tok);           // panel still holds the shared copy
        REQUIRE(*tok == false); // subject death observed through the retained token
    }

    // Step 7: the real production teardown+rebuild path must not crash. It resets
    // ObserverGuards whose subjects were already freed in step 5; ObserverGuard::
    // reset() must skip lv_observer_remove() because the token is dead. After the
    // rebuild the only remaining fan is PART_COOLING, so there are no secondary
    // rows/observers/lifetimes — the vectors come back empty and aligned.
    lv_obj_t* list = lv_obj_create(lv_screen_active());
    REQUIRE(list != nullptr);
    ControlsPanelTestAccess::set_fans_list(panel, list);

    REQUIRE_NOTHROW(ControlsPanelTestAccess::populate(panel));

    REQUIRE(ControlsPanelTestAccess::rows(panel).empty());
    REQUIRE(ControlsPanelTestAccess::observers(panel).empty());
    REQUIRE(ControlsPanelTestAccess::lifetimes(panel).empty());

    // Flush any deferred observer notifications before the panel + fixture tear
    // down, so no queued callback references the about-to-be-destroyed panel.
    helix::ui::UpdateQueue::instance().drain();
}
