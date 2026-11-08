// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_tracker_gating.cpp
 * @brief Phase 8 gating-matrix + mid-print rebaseline tests.
 *
 * Exercises FilamentConsumptionTracker's per-slot gating policy at the
 * integration level (tracker + AmsSlotSink + AmsBackendMock together).
 * Sibling unit-level tests in test_consumption_sink_ams.cpp exercise the
 * gating directly on AmsSlotSink; this file ensures the tracker respects
 * that gating end-to-end when routing per-extruder deltas.
 *
 * Matrix covered (design §4 "Gating policy"):
 *   - Spoolman-linked slot (spoolman_id != 0) is skipped.
 *   - Backend declaring native tracking is skipped.
 *   - Mid-print user edit of remaining_weight_g rebaselines the sink rather
 *     than decrementing through the new value.
 *   - Unknown-weight slot (remaining_weight_g == -1) stays untracked;
 *     becomes trackable after the user sets a weight and the sink is
 *     re-registered (the production path when backend slot config churns).
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/backend_user_edit.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_globals.h"
#include "consumption_sink.h"
#include "filament_consumption_tracker.h"
#include "filament_consumption_tracker_test_access.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

using helix::AmsSlotSink;
using helix::FilamentConsumptionTracker;
using helix::FilamentConsumptionTrackerTestAccess;
using helix::PrintJobState;
using helix::SinkKind;

namespace {

// Mirrors TrackerRoutingFixture in test_tracker_routing.cpp — 4-slot mock
// with identity extruder→slot mapping, all slots trackable-by-default, the
// tracker started and forced into PRINTING. Kept in-file so routing vs
// gating tests don't cross-pollinate their setup invariants.
struct TrackerGatingFixture : LVGLTestFixture {
    int backend_idx = 0;
    helix::AmsBackendMock* mock = nullptr;

    TrackerGatingFixture() {
        auto& ams = helix::AmsState::instance();
        auto& printer = get_printer_state();

        FilamentConsumptionTracker::instance().stop();
        ams.clear_backends();
        ams.deinit_subjects();
        ams.init_subjects(false);
        printer.init_subjects(false);
        ams.clear_external_spool_info();

        auto m = std::make_unique<helix::AmsBackendMock>(4);
        m->set_identity_extruder_mapping_for_testing(true);
        mock = m.get();
        backend_idx = ams.add_backend(std::move(m));

        for (int s = 0; s < 4; ++s) {
            helix::SlotInfo info = mock->get_slot_info(s);
            info.material = "PLA";
            info.remaining_weight_g = 1000.0f;
            info.total_weight_g = 1000.0f;
            info.spoolman_id = 0;
            helix::test::apply_edit(*mock, s, info);
        }

        // Zero stale subjects before tracker start — see routing fixture
        // notes: LVGL subjects notify observers on add with the existing
        // value, which would consume filament from the wrong slot.
        lv_subject_set_int(printer.print_state().get_print_filament_used_subject(), 0);
        SubjectLifetime reset_lt;
        for (int i = 0; i < 16; ++i) {
            auto* subj = printer.print_state().get_extruder_filament_used_subject(i, reset_lt);
            if (subj) {
                lv_subject_set_int(subj, 0);
            }
        }
        helix::ui::UpdateQueue::instance().drain();

        FilamentConsumptionTracker::instance().start();
        FilamentConsumptionTrackerTestAccess::force_print_state(PrintJobState::PRINTING);
        helix::ui::UpdateQueue::instance().drain();
    }

    ~TrackerGatingFixture() override {
        FilamentConsumptionTrackerTestAccess::force_print_state(PrintJobState::COMPLETE);
        FilamentConsumptionTracker::instance().stop();
        auto& ams = helix::AmsState::instance();
        ams.clear_backends();
        ams.clear_external_spool_info();
    }
};

} // namespace

TEST_CASE_METHOD(TrackerGatingFixture, "Gating: slot with spoolman_id != 0 not tracked",
                 "[tracker][gating]") {
    // Link slot 0 to Spoolman BEFORE any delta arrives so the tracker's
    // per-tick re-gate (apply_delta top-of-loop) sees it and disables the
    // sink without decrementing.
    helix::SlotInfo info = mock->get_slot_info(0);
    info.spoolman_id = 42;
    helix::test::apply_edit(*mock, 0, info);

    auto& state = get_printer_state();
    SubjectLifetime lt;
    auto* e0 = state.print_state().get_extruder_filament_used_subject(0, lt);
    REQUIRE(e0 != nullptr);

    const float before = mock->get_slot_info(0).remaining_weight_g;
    lv_subject_set_int(e0, 1000);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(mock->get_slot_info(0).remaining_weight_g == before);
    // Siblings unaffected.
    CHECK(mock->get_slot_info(1).remaining_weight_g == 1000.0f);
}

TEST_CASE_METHOD(TrackerGatingFixture, "Gating: native tracking backend not decremented",
                 "[tracker][gating]") {
    // Flip the backend into "tracks natively" mode and re-snapshot by
    // cycling through a fresh PRINTING edge. snapshot() checks
    // tracks_consumption_natively() and marks every sink inactive.
    mock->set_tracks_consumption_natively_for_testing(true);
    FilamentConsumptionTrackerTestAccess::force_print_state(PrintJobState::COMPLETE);
    FilamentConsumptionTrackerTestAccess::force_print_state(PrintJobState::PRINTING);
    helix::ui::UpdateQueue::instance().drain();

    auto& state = get_printer_state();
    SubjectLifetime lt;
    auto* e0 = state.print_state().get_extruder_filament_used_subject(0, lt);
    REQUIRE(e0 != nullptr);

    const float before0 = mock->get_slot_info(0).remaining_weight_g;
    const float before1 = mock->get_slot_info(1).remaining_weight_g;

    lv_subject_set_int(e0, 1000);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(mock->get_slot_info(0).remaining_weight_g == before0);
    CHECK(mock->get_slot_info(1).remaining_weight_g == before1);
}

TEST_CASE_METHOD(TrackerGatingFixture, "Mid-print edit rebaselines sink", "[tracker][gating]") {
    auto& state = get_printer_state();
    SubjectLifetime lt;
    auto* e0 = state.print_state().get_extruder_filament_used_subject(0, lt);
    REQUIRE(e0 != nullptr);

    // First tick: push 500 mm → slot 0 decrements ~1.49 g.
    lv_subject_set_int(e0, 500);
    helix::ui::UpdateQueue::instance().drain();
    const float after_first = mock->get_slot_info(0).remaining_weight_g;
    CHECK(after_first < 1000.0f);
    CHECK(after_first > 998.0f);

    // User edits mid-print to 800 g (delta from last_written_weight_g_
    // exceeds the 0.5 g rebaseline threshold).
    helix::SlotInfo info = mock->get_slot_info(0);
    info.remaining_weight_g = 800.0f;
    helix::test::apply_edit(*mock, 0, info);

    // Next tick: sink detects external write and rebases — no decrement on
    // this tick, weight stays at exactly the user's 800 g.
    lv_subject_set_int(e0, 600);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(mock->get_slot_info(0).remaining_weight_g == 800.0f);

    // Further extrusion (+1000 mm past the rebase point) decrements from 800.
    lv_subject_set_int(e0, 1600);
    helix::ui::UpdateQueue::instance().drain();
    const float after_third = mock->get_slot_info(0).remaining_weight_g;
    CHECK(after_third < 800.0f);
    CHECK(after_third > 796.0f);
}

TEST_CASE_METHOD(TrackerGatingFixture,
                 "Gating: unknown weight becomes trackable when set mid-print",
                 "[tracker][gating]") {
    // Start slot 0 with unknown weight. The fixture's default snapshot at
    // PRINTING-edge already ran with weight=1000, so we force a fresh
    // snapshot cycle with the new -1 to land the sink in the inactive state.
    helix::SlotInfo info = mock->get_slot_info(0);
    info.remaining_weight_g = -1.0f;
    helix::test::apply_edit(*mock, 0, info);
    FilamentConsumptionTrackerTestAccess::force_print_state(PrintJobState::COMPLETE);
    FilamentConsumptionTrackerTestAccess::force_print_state(PrintJobState::PRINTING);
    helix::ui::UpdateQueue::instance().drain();

    auto& state = get_printer_state();
    SubjectLifetime lt;
    auto* e0 = state.print_state().get_extruder_filament_used_subject(0, lt);
    REQUIRE(e0 != nullptr);

    // Push delta on an inactive slot → no write, still -1 (sentinel).
    lv_subject_set_int(e0, 500);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(mock->get_slot_info(0).remaining_weight_g == -1.0f);

    // User sets the weight mid-print. The inactive sink won't re-evaluate
    // via apply_delta alone (snapshot() is the gate that checks weight>=0);
    // the production path re-registers sinks on backend config churn. We
    // simulate that by cycling print state, which re-snapshots all sinks.
    info = mock->get_slot_info(0);
    info.remaining_weight_g = 800.0f;
    helix::test::apply_edit(*mock, 0, info);

    // Push another delta BEFORE re-snapshot — still inactive, no write.
    lv_subject_set_int(e0, 1000);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(mock->get_slot_info(0).remaining_weight_g == 800.0f);

    // Cycle print state → sink re-snapshots with the now-valid weight.
    FilamentConsumptionTrackerTestAccess::force_print_state(PrintJobState::COMPLETE);
    FilamentConsumptionTrackerTestAccess::force_print_state(PrintJobState::PRINTING);
    helix::ui::UpdateQueue::instance().drain();

    // Re-zero the extruder subject so the next push is a clean delta from
    // the freshly-taken baseline. (PRINTING edge snapshots at the current
    // aggregate mm; the per-extruder reset branch in apply_delta handles
    // the mm going "backwards" on the first tick of the new cycle.)
    lv_subject_set_int(e0, 0);
    helix::ui::UpdateQueue::instance().drain();

    // Third delta — slot should now decrement from 800 g.
    lv_subject_set_int(e0, 1000);
    helix::ui::UpdateQueue::instance().drain();
    const float after = mock->get_slot_info(0).remaining_weight_g;
    CHECK(after < 800.0f);
    CHECK(after > 796.0f);
}
