// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

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

namespace {

// Sets up an AmsState singleton with a single AmsBackendMock (4 slots) so the
// sink can read/write real SlotInfo via the backend's set_slot_info API.
struct AmsSlotSinkFixture : LVGLTestFixture {
    int backend_idx = 0;
    AmsBackendMock* mock = nullptr;

    AmsSlotSinkFixture() {
        auto& ams = AmsState::instance();
        auto& printer = get_printer_state();
        ams.clear_backends();
        ams.deinit_subjects();
        // AmsState::init_subjects installs an observer on the global
        // PrinterState's print_state_enum subject — that subject must exist
        // before init_subjects runs, otherwise observe_int_sync attaches to an
        // uninitialized lv_subject_t. (Crashes on macOS where every test
        // shares one process; on Linux nightly the parallel shards happen to
        // have a prior test that already initialized PrinterState.)
        printer.init_subjects(false);
        ams.init_subjects(false);

        auto m = std::make_unique<AmsBackendMock>(4);
        mock = m.get();
        backend_idx = ams.add_backend(std::move(m));

        // Seed slot 0 with a known, trackable configuration.
        SlotInfo info = mock->get_slot_info(0);
        info.material = "PLA";
        info.remaining_weight_g = 500.0f;
        info.total_weight_g = 1000.0f;
        info.spoolman_id = 0;
        mock->set_slot_info(0, info, /*persist=*/false);
    }

    ~AmsSlotSinkFixture() override {
        auto& ams = AmsState::instance();
        ams.clear_backends();
        ams.deinit_subjects();
    }
};

} // namespace

TEST_CASE_METHOD(AmsSlotSinkFixture,
                 "AmsSlotSink: snapshot when weight known and no spoolman link is trackable",
                 "[consumption_sink][ams]") {
    AmsSlotSink sink(backend_idx, 0);
    sink.snapshot(0.0f);
    REQUIRE(sink.is_trackable());
}

TEST_CASE_METHOD(AmsSlotSinkFixture, "AmsSlotSink: skipped when remaining_weight_g is unknown",
                 "[consumption_sink][ams]") {
    SlotInfo info = mock->get_slot_info(0);
    info.remaining_weight_g = -1.0f;
    mock->set_slot_info(0, info, false);

    AmsSlotSink sink(backend_idx, 0);
    sink.snapshot(0.0f);
    REQUIRE_FALSE(sink.is_trackable());
}

TEST_CASE_METHOD(AmsSlotSinkFixture, "AmsSlotSink: skipped when spoolman_id is set",
                 "[consumption_sink][ams]") {
    SlotInfo info = mock->get_slot_info(0);
    info.spoolman_id = 42;
    mock->set_slot_info(0, info, false);

    AmsSlotSink sink(backend_idx, 0);
    sink.snapshot(0.0f);
    REQUIRE_FALSE(sink.is_trackable());
}

TEST_CASE_METHOD(AmsSlotSinkFixture, "AmsSlotSink: skipped when material density unresolvable",
                 "[consumption_sink][ams]") {
    SlotInfo info = mock->get_slot_info(0);
    info.material = "UnknownNovelMaterial9000";
    mock->set_slot_info(0, info, false);

    AmsSlotSink sink(backend_idx, 0);
    sink.snapshot(0.0f);
    REQUIRE_FALSE(sink.is_trackable());
}

TEST_CASE_METHOD(AmsSlotSinkFixture, "AmsSlotSink: skipped when backend declares native tracking",
                 "[consumption_sink][ams]") {
    mock->set_tracks_consumption_natively_for_testing(true);

    AmsSlotSink sink(backend_idx, 0);
    sink.snapshot(0.0f);
    REQUIRE_FALSE(sink.is_trackable());
}

TEST_CASE_METHOD(AmsSlotSinkFixture, "AmsSlotSink: apply_delta decrements remaining_weight_g",
                 "[consumption_sink][ams]") {
    AmsSlotSink sink(backend_idx, 0);
    sink.snapshot(0.0f);

    // 1000mm of 1.75mm PLA at 1.24 g/cm^3 ≈ 2.98 g consumed.
    sink.apply_delta(1000.0f);
    SlotInfo after = mock->get_slot_info(0);
    REQUIRE(after.remaining_weight_g < 500.0f);
    REQUIRE(after.remaining_weight_g > 496.0f);
}

TEST_CASE_METHOD(AmsSlotSinkFixture, "AmsSlotSink: apply_delta clamps remaining at zero",
                 "[consumption_sink][ams]") {
    SlotInfo seed = mock->get_slot_info(0);
    seed.remaining_weight_g = 5.0f;
    mock->set_slot_info(0, seed, false);

    AmsSlotSink sink(backend_idx, 0);
    sink.snapshot(0.0f);
    REQUIRE(sink.is_trackable());

    // Consume way more than 5 g worth of filament.
    sink.apply_delta(10000.0f);
    SlotInfo after = mock->get_slot_info(0);
    REQUIRE(after.remaining_weight_g == 0.0f);
}

TEST_CASE_METHOD(AmsSlotSinkFixture, "AmsSlotSink: other slots untouched by neighbor's apply_delta",
                 "[consumption_sink][ams]") {
    // Seed slot 1 with a known weight so we can confirm it stays.
    SlotInfo info1 = mock->get_slot_info(1);
    info1.material = "PLA";
    info1.remaining_weight_g = 750.0f;
    info1.total_weight_g = 1000.0f;
    info1.spoolman_id = 0;
    mock->set_slot_info(1, info1, false);

    AmsSlotSink sink0(backend_idx, 0);
    sink0.snapshot(0.0f);
    sink0.apply_delta(1000.0f);

    REQUIRE(mock->get_slot_info(1).remaining_weight_g == 750.0f);
}

TEST_CASE_METHOD(AmsSlotSinkFixture, "AmsSlotSink: external write mid-tick rebaselines",
                 "[consumption_sink][ams]") {
    AmsSlotSink sink(backend_idx, 0);
    sink.snapshot(0.0f);
    sink.apply_delta(1000.0f); // ~2.98 g -> ~497 g remaining

    // Simulate a user or Spoolman writing a new authoritative value.
    SlotInfo info = mock->get_slot_info(0);
    info.remaining_weight_g = 300.0f;
    mock->set_slot_info(0, info, false);

    // Next tick should detect the mismatch and rebaseline (no decrement).
    sink.apply_delta(1100.0f);
    REQUIRE(mock->get_slot_info(0).remaining_weight_g == 300.0f);

    // Further extrusion decrements from 300 g.
    sink.apply_delta(2100.0f); // 1000 mm past rebase
    SlotInfo after = mock->get_slot_info(0);
    REQUIRE(after.remaining_weight_g < 300.0f);
    REQUIRE(after.remaining_weight_g > 296.0f);
}

TEST_CASE_METHOD(AmsSlotSinkFixture,
                 "AmsSlotSink: apply_delta gates mid-stream when spoolman_id appears",
                 "[consumption_sink][ams]") {
    AmsSlotSink sink(backend_idx, 0);
    sink.snapshot(0.0f);
    REQUIRE(sink.is_trackable());

    // User links the slot to Spoolman mid-print.
    SlotInfo info = mock->get_slot_info(0);
    info.spoolman_id = 100;
    mock->set_slot_info(0, info, false);

    float before = mock->get_slot_info(0).remaining_weight_g;
    sink.apply_delta(1000.0f);
    REQUIRE_FALSE(sink.is_trackable());
    REQUIRE(mock->get_slot_info(0).remaining_weight_g == before);
}

// ---------------------------------------------------------------------------
// Tracker sink registry
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(LVGLTestFixture,
                 "FilamentConsumptionTracker: register_sink returns a usable handle",
                 "[consumption_sink][ams][tracker_registry]") {
    auto& ams = AmsState::instance();
    ams.clear_backends();
    ams.deinit_subjects();
    ams.init_subjects(false);

    auto& tracker = FilamentConsumptionTracker::instance();
    auto sink = std::make_unique<helix::AmsSlotSink>(0, 0);
    helix::AmsSlotSink* raw = sink.get();
    auto handle = tracker.register_sink(std::move(sink));
    REQUIRE(handle == raw);
    tracker.unregister_sink(handle);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "AmsState: add_backend registers one slot sink per slot; clear unregisters",
                 "[consumption_sink][ams][tracker_registry]") {
    auto& ams = AmsState::instance();
    ams.clear_backends();
    ams.deinit_subjects();
    ams.init_subjects(false);

    constexpr int SLOTS = 4;
    auto m = std::make_unique<AmsBackendMock>(SLOTS);
    int idx = ams.add_backend(std::move(m));
    REQUIRE(idx == 0);

    // After add_backend, SLOTS sinks should have been registered.
    REQUIRE(FilamentConsumptionTrackerTestAccess::sink_count() >= static_cast<std::size_t>(SLOTS));

    ams.clear_backends();
    // After clear_backends, slot sinks should be gone (but an external sink may
    // still be present if start() ran — we only check no leftover AMS sinks).
    REQUIRE(FilamentConsumptionTrackerTestAccess::ams_sink_count() == 0);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "Tracker: mid-print register_sink auto-snapshots and tracks deltas",
                 "[consumption_sink][ams][registration]") {
    auto& ams = AmsState::instance();
    auto& printer = get_printer_state();
    auto& tracker = FilamentConsumptionTracker::instance();

    ams.clear_backends();
    ams.deinit_subjects();
    ams.init_subjects(false);
    printer.init_subjects(false);
    ams.clear_external_spool_info();

    // Backend gets added pre-start so its per-slot sinks are registered while
    // no print is in progress. Seed slot 0 with a trackable configuration.
    auto m = std::make_unique<AmsBackendMock>(4);
    AmsBackendMock* mock = m.get();
    int backend_idx = ams.add_backend(std::move(m));
    SlotInfo seed = mock->get_slot_info(0);
    seed.material = "PLA";
    seed.remaining_weight_g = 500.0f;
    seed.total_weight_g = 1000.0f;
    seed.spoolman_id = 0;
    mock->set_slot_info(0, seed, /*persist=*/false);

    // Start tracker and drive the printer into PRINTING so print_in_progress_
    // is true when we register a new sink below.
    tracker.start();
    lv_subject_set_int(printer.get_print_filament_used_subject(), 0);
    lv_subject_set_int(printer.get_print_state_enum_subject(),
                       static_cast<int>(helix::PrintJobState::PRINTING));
    helix::ui::UpdateQueue::instance().drain();

    // Register a fresh sink mid-print. Tracker should snapshot it immediately.
    SlotInfo seed1 = mock->get_slot_info(1);
    seed1.material = "PLA";
    seed1.remaining_weight_g = 800.0f;
    seed1.total_weight_g = 1000.0f;
    seed1.spoolman_id = 0;
    mock->set_slot_info(1, seed1, /*persist=*/false);

    auto late_sink = std::make_unique<helix::AmsSlotSink>(backend_idx, 1);
    helix::AmsSlotSink* raw = late_sink.get();
    auto handle = tracker.register_sink(std::move(late_sink));
    REQUIRE(handle == raw);
    REQUIRE(raw->is_trackable());

    // Post-Phase-5: the aggregate filament_used path only decrements the slot
    // whose index matches the backend's get_current_slot(). Point the mock at
    // slot 1 before pushing the delta so the late-registered sink receives it.
    mock->start();
    REQUIRE(mock->select_slot(1).success());
    REQUIRE(mock->get_current_slot() == 1);

    // Push a filament_used delta and verify the late sink's slot decremented.
    lv_subject_set_int(printer.get_print_filament_used_subject(), 1000);
    helix::ui::UpdateQueue::instance().drain();

    SlotInfo after = mock->get_slot_info(1);
    // 1000mm PLA @ 1.75mm / 1.24 g/cm^3 ≈ 2.98 g.
    REQUIRE(after.remaining_weight_g < 800.0f);
    REQUIRE(after.remaining_weight_g > 796.0f);

    // Clean up.
    lv_subject_set_int(printer.get_print_state_enum_subject(),
                       static_cast<int>(helix::PrintJobState::COMPLETE));
    helix::ui::UpdateQueue::instance().drain();
    tracker.stop();
    ams.clear_backends();
    ams.clear_external_spool_info();
}
