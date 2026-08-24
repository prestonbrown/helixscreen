// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_tracker_routing.cpp
 * @brief Phase 5 tests: per-extruder consumption routing.
 *
 * Exercises FilamentConsumptionTracker's dispatch of per-extruder
 * filament_used_mm deltas to the AmsSlotSink whose backend declares a
 * slot_for_extruder() mapping for that extruder index.
 *
 * Scope boundaries:
 *   - Mock's identity mapping (extruder N -> slot N) stands in for the
 *     production overrides on Snapmaker/Toolchanger (Phase 6, out of scope).
 *   - Test drives extruder subjects via the printer state's dynamic subject
 *     pool; the tracker's own SubjectLifetime is independent of the test's.
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
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

struct TrackerRoutingFixture : LVGLTestFixture {
    int backend_idx = 0;
    AmsBackendMock* mock = nullptr;

    TrackerRoutingFixture() {
        auto& ams = AmsState::instance();
        auto& printer = get_printer_state();

        // Full teardown: the tracker is a singleton whose sink registry
        // carries over between tests. Stop it, clear AMS sinks, reset subjects.
        FilamentConsumptionTracker::instance().stop();
        ams.clear_backends();
        ams.deinit_subjects();
        ams.init_subjects(false);
        printer.init_subjects(false);
        ams.clear_external_spool_info();

        // Build a 4-slot mock with identity tool->slot mapping (extruder N == slot N).
        auto m = std::make_unique<AmsBackendMock>(4);
        m->set_identity_extruder_mapping_for_testing(true);
        mock = m.get();
        backend_idx = ams.add_backend(std::move(m));

        // Seed every slot with a known, trackable configuration so per-slot
        // sinks become active on snapshot().
        for (int s = 0; s < 4; ++s) {
            SlotInfo info = mock->get_slot_info(s);
            info.material = "PLA";
            info.remaining_weight_g = 1000.0f;
            info.total_weight_g = 1000.0f;
            info.spoolman_id = 0;
            mock->set_slot_info(s, info, /*persist=*/false);
        }

        // Zero the aggregate + per-extruder subjects. PrinterPrintState
        // pre-populates MAX_EXTRUDER_SCAN subjects that persist across tests;
        // stale values from a prior test would fire the observers immediately
        // on registration with the OLD value (LVGL subjects notify on add),
        // consuming filament from the wrong slot before print-start snapshot.
        lv_subject_set_int(printer.get_print_filament_used_subject(), 0);
        SubjectLifetime reset_lt;
        for (int i = 0; i < 16; ++i) {
            auto* subj = printer.get_extruder_filament_used_subject(i, reset_lt);
            if (subj) {
                lv_subject_set_int(subj, 0);
            }
        }
        helix::ui::UpdateQueue::instance().drain();

        // Start tracker; drive into PRINTING so sinks snapshot.
        FilamentConsumptionTracker::instance().start();
        FilamentConsumptionTrackerTestAccess::force_print_state(PrintJobState::PRINTING);
        helix::ui::UpdateQueue::instance().drain();
    }

    ~TrackerRoutingFixture() override {
        FilamentConsumptionTrackerTestAccess::force_print_state(PrintJobState::COMPLETE);
        FilamentConsumptionTracker::instance().stop();
        auto& ams = AmsState::instance();
        ams.clear_backends();
        ams.clear_external_spool_info();
    }
};

} // namespace

TEST_CASE_METHOD(TrackerRoutingFixture, "Per-extruder delta routes only to the mapped slot",
                 "[tracker][routing]") {
    auto& state = get_printer_state();
    SubjectLifetime lifetime;
    auto* e1 = state.get_extruder_filament_used_subject(1, lifetime);
    REQUIRE(e1 != nullptr);

    // Push 1000 mm on extruder1 -> slot 1 should decrement; others unchanged.
    lv_subject_set_int(e1, 1000);
    helix::ui::UpdateQueue::instance().drain();

    // 1000 mm of 1.75 mm PLA @ 1.24 g/cm^3 ~= 2.98 g consumed.
    CHECK(mock->get_slot_info(0).remaining_weight_g == 1000.0f);
    CHECK(mock->get_slot_info(1).remaining_weight_g < 1000.0f);
    CHECK(mock->get_slot_info(1).remaining_weight_g > 996.0f);
    CHECK(mock->get_slot_info(2).remaining_weight_g == 1000.0f);
    CHECK(mock->get_slot_info(3).remaining_weight_g == 1000.0f);
}

TEST_CASE_METHOD(TrackerRoutingFixture,
                 "Aggregate delta skips mapped backend (per-extruder path owns it)",
                 "[tracker][routing]") {
    // When a backend declares any slot_for_extruder() mapping, the aggregate
    // `print_stats.filament_used` stream must NOT decrement that backend's
    // sinks — otherwise we double-count with the per-extruder path.
    auto& state = get_printer_state();

    const float before0 = mock->get_slot_info(0).remaining_weight_g;
    const float before1 = mock->get_slot_info(1).remaining_weight_g;

    // Drive the aggregate subject (no per-extruder subject change).
    lv_subject_set_int(state.get_print_filament_used_subject(), 1000);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(mock->get_slot_info(0).remaining_weight_g == before0);
    CHECK(mock->get_slot_info(1).remaining_weight_g == before1);
}

TEST_CASE_METHOD(TrackerRoutingFixture, "Per-extruder dispatch ignores unmapped extruder indices",
                 "[tracker][routing]") {
    auto& state = get_printer_state();
    SubjectLifetime lifetime;
    // extruder index 8 is past the 4-slot mock's range: slot_for_extruder
    // returns nullopt and no sink should decrement.
    auto* e8 = state.get_extruder_filament_used_subject(8, lifetime);
    REQUIRE(e8 != nullptr);

    lv_subject_set_int(e8, 1000);
    helix::ui::UpdateQueue::instance().drain();

    for (int s = 0; s < 4; ++s) {
        CHECK(mock->get_slot_info(s).remaining_weight_g == 1000.0f);
    }
}

// ---------------------------------------------------------------------------
// Single-extruder multi-slot backend: aggregate path with current_slot routing
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(LVGLTestFixture,
                 "Aggregate routing: single-extruder multi-slot decrements only current_slot",
                 "[tracker][routing]") {
    auto& ams = AmsState::instance();
    auto& printer = get_printer_state();

    FilamentConsumptionTracker::instance().stop();
    ams.clear_backends();
    ams.deinit_subjects();
    ams.init_subjects(false);
    printer.init_subjects(false);
    ams.clear_external_spool_info();

    // Mock WITHOUT identity mapping: simulates HappyHare/CFS/ACE/IFS — one
    // extruder, multiple slots, active slot reported by get_current_slot().
    auto m = std::make_unique<AmsBackendMock>(4);
    AmsBackendMock* mock = m.get();
    // Must start the mock so select_slot() is accepted (mock gate).
    mock->start();
    int idx = ams.add_backend(std::move(m));
    REQUIRE(idx == 0);

    for (int s = 0; s < 4; ++s) {
        SlotInfo info = mock->get_slot_info(s);
        info.material = "PLA";
        info.remaining_weight_g = 1000.0f;
        info.total_weight_g = 1000.0f;
        info.spoolman_id = 0;
        mock->set_slot_info(s, info, /*persist=*/false);
    }

    // select_slot on the mock synchronously updates current_slot (no motion).
    REQUIRE(mock->select_slot(2).success());
    REQUIRE(mock->get_current_slot() == 2);

    // Zero subjects so stale cross-test values don't fire on observer add.
    lv_subject_set_int(printer.get_print_filament_used_subject(), 0);
    SubjectLifetime reset_lt;
    for (int i = 0; i < 16; ++i) {
        auto* subj = printer.get_extruder_filament_used_subject(i, reset_lt);
        if (subj) {
            lv_subject_set_int(subj, 0);
        }
    }
    helix::ui::UpdateQueue::instance().drain();

    FilamentConsumptionTracker::instance().start();
    FilamentConsumptionTrackerTestAccess::force_print_state(PrintJobState::PRINTING);
    helix::ui::UpdateQueue::instance().drain();

    // Push aggregate delta; only slot 2 (current_slot) should decrement.
    lv_subject_set_int(printer.get_print_filament_used_subject(), 1000);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(mock->get_slot_info(0).remaining_weight_g == 1000.0f);
    CHECK(mock->get_slot_info(1).remaining_weight_g == 1000.0f);
    CHECK(mock->get_slot_info(2).remaining_weight_g < 1000.0f);
    CHECK(mock->get_slot_info(2).remaining_weight_g > 996.0f);
    CHECK(mock->get_slot_info(3).remaining_weight_g == 1000.0f);

    FilamentConsumptionTrackerTestAccess::force_print_state(PrintJobState::COMPLETE);
    FilamentConsumptionTracker::instance().stop();
    ams.clear_backends();
    ams.clear_external_spool_info();
}
