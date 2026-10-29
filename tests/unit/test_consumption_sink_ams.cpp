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

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::AmsSlotSink;
using helix::FilamentConsumptionTracker;
using helix::FilamentConsumptionTrackerTestAccess;

namespace {

// Sets up an AmsState singleton with a single AmsBackendMock (4 slots) so the
// sink can read/write real SlotInfo via the backend's sync_external_identity API.
struct AmsSlotSinkFixture : LVGLTestFixture {
    int backend_idx = 0;
    helix::AmsBackendMock* mock = nullptr;

    AmsSlotSinkFixture() {
        auto& ams = helix::AmsState::instance();
        auto& printer = get_printer_state();
        ams.clear_backends();
        ams.deinit_subjects();
        // AmsState::init_subjects installs an observer on the global
        // PrinterState's print_state_enum subject — that subject must exist
        // before init_subjects runs, otherwise observe<int> attaches to an
        // uninitialized lv_subject_t. (Crashes on macOS where every test
        // shares one process; on Linux nightly the parallel shards happen to
        // have a prior test that already initialized PrinterState.)
        printer.init_subjects(false);
        ams.init_subjects(false);

        auto m = std::make_unique<helix::AmsBackendMock>(4);
        mock = m.get();
        backend_idx = ams.add_backend(std::move(m));

        // Seed slot 0 with a known, trackable configuration.
        helix::SlotInfo info = mock->get_slot_info(0);
        info.material = "PLA";
        info.remaining_weight_g = 500.0f;
        info.total_weight_g = 1000.0f;
        info.spoolman_id = 0;
        mock->sync_external_identity(0, info);
    }

    ~AmsSlotSinkFixture() override {
        auto& ams = helix::AmsState::instance();
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
    helix::SlotInfo info = mock->get_slot_info(0);
    info.remaining_weight_g = -1.0f;
    mock->sync_external_identity(0, info);

    AmsSlotSink sink(backend_idx, 0);
    sink.snapshot(0.0f);
    REQUIRE_FALSE(sink.is_trackable());
}

TEST_CASE_METHOD(AmsSlotSinkFixture, "AmsSlotSink: skipped when spoolman_id is set",
                 "[consumption_sink][ams]") {
    helix::SlotInfo info = mock->get_slot_info(0);
    info.spoolman_id = 42;
    mock->sync_external_identity(0, info);

    AmsSlotSink sink(backend_idx, 0);
    sink.snapshot(0.0f);
    REQUIRE_FALSE(sink.is_trackable());
}

TEST_CASE_METHOD(AmsSlotSinkFixture, "AmsSlotSink: skipped when material density unresolvable",
                 "[consumption_sink][ams]") {
    helix::SlotInfo info = mock->get_slot_info(0);
    info.material = "UnknownNovelMaterial9000";
    mock->sync_external_identity(0, info);

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
    helix::SlotInfo after = mock->get_slot_info(0);
    REQUIRE(after.remaining_weight_g < 500.0f);
    REQUIRE(after.remaining_weight_g > 496.0f);
}

TEST_CASE_METHOD(AmsSlotSinkFixture, "consumption sinks meter at the printer's filament diameter",
                 "[consumption_sink][filament_diameter]") {
    helix::PrinterDiscovery hw;
    hw.parse_extruder_dimensions(nlohmann::json{{"extruder", {{"filament_diameter", 2.85}}}});
    get_printer_state().set_hardware(hw);

    // 1000mm of 2.85mm PLA at 1.24 g/cm^3 is 6.379 cm^3, 7.91 g.
    SECTION("AMS slot") {
        AmsSlotSink sink(backend_idx, 0);
        sink.snapshot(0.0f);
        sink.apply_delta(1000.0f);
        CHECK(mock->get_slot_info(0).remaining_weight_g ==
              Catch::Approx(500.0f - 7.91f).epsilon(1e-3));
    }
    SECTION("external spool") {
        helix::SlotInfo info;
        info.material = "PLA";
        info.remaining_weight_g = 500.0f;
        info.total_weight_g = 1000.0f;
        helix::AmsState::instance().set_external_spool_info_in_memory(info);

        helix::ExternalSpoolSink sink;
        sink.snapshot(0.0f);
        sink.apply_delta(1000.0f);
        auto after = helix::AmsState::instance().get_external_spool_info();
        REQUIRE(after.has_value());
        CHECK(after->remaining_weight_g == Catch::Approx(500.0f - 7.91f).epsilon(1e-3));
        helix::AmsState::instance().clear_external_spool_info();
    }
}

TEST_CASE_METHOD(AmsSlotSinkFixture, "AmsSlotSink: apply_delta clamps remaining at zero",
                 "[consumption_sink][ams]") {
    helix::SlotInfo seed = mock->get_slot_info(0);
    seed.remaining_weight_g = 5.0f;
    mock->sync_external_identity(0, seed);

    AmsSlotSink sink(backend_idx, 0);
    sink.snapshot(0.0f);
    REQUIRE(sink.is_trackable());

    // Consume way more than 5 g worth of filament.
    sink.apply_delta(10000.0f);
    helix::SlotInfo after = mock->get_slot_info(0);
    REQUIRE(after.remaining_weight_g == 0.0f);
}

TEST_CASE_METHOD(AmsSlotSinkFixture, "AmsSlotSink: other slots untouched by neighbor's apply_delta",
                 "[consumption_sink][ams]") {
    // Seed slot 1 with a known weight so we can confirm it stays.
    helix::SlotInfo info1 = mock->get_slot_info(1);
    info1.material = "PLA";
    info1.remaining_weight_g = 750.0f;
    info1.total_weight_g = 1000.0f;
    info1.spoolman_id = 0;
    mock->sync_external_identity(1, info1);

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
    helix::SlotInfo info = mock->get_slot_info(0);
    info.remaining_weight_g = 300.0f;
    mock->sync_external_identity(0, info);

    // Next tick should detect the mismatch and rebaseline (no decrement).
    sink.apply_delta(1100.0f);
    REQUIRE(mock->get_slot_info(0).remaining_weight_g == 300.0f);

    // Further extrusion decrements from 300 g.
    sink.apply_delta(2100.0f); // 1000 mm past rebase
    helix::SlotInfo after = mock->get_slot_info(0);
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
    helix::SlotInfo info = mock->get_slot_info(0);
    info.spoolman_id = 100;
    mock->sync_external_identity(0, info);

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
    auto& ams = helix::AmsState::instance();
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
    auto& ams = helix::AmsState::instance();
    ams.clear_backends();
    ams.deinit_subjects();
    ams.init_subjects(false);

    constexpr int kSlots = 4;
    auto m = std::make_unique<helix::AmsBackendMock>(kSlots);
    int idx = ams.add_backend(std::move(m));
    REQUIRE(idx == 0);

    // After add_backend, kSlots sinks should have been registered.
    REQUIRE(FilamentConsumptionTrackerTestAccess::sink_count() >= static_cast<std::size_t>(kSlots));

    ams.clear_backends();
    // After clear_backends, slot sinks should be gone (but an external sink may
    // still be present if start() ran — we only check no leftover AMS sinks).
    REQUIRE(FilamentConsumptionTrackerTestAccess::ams_sink_count() == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "AmsState: slots a backend reports after add get sinks",
                 "[consumption_sink][ams][tracker_registry]") {
    auto& ams = helix::AmsState::instance();
    ams.clear_backends();
    ams.deinit_subjects();
    ams.init_subjects(false);

    auto m = std::make_unique<helix::AmsBackendMock>(4);
    helix::AmsBackendMock* mock = m.get();
    REQUIRE(ams.add_backend(std::move(m)) == 0);
    REQUIRE(FilamentConsumptionTrackerTestAccess::ams_sink_count() == 4);

    mock->set_multi_unit_mode(true);
    const int grown = mock->get_system_info().total_slots;
    REQUIRE(grown > 4);
    ams.sync_backend(0);
    CHECK(FilamentConsumptionTrackerTestAccess::ams_sink_count() ==
          static_cast<std::size_t>(grown));

    // A second sync with no new slots registers nothing more.
    ams.sync_backend(0);
    CHECK(FilamentConsumptionTrackerTestAccess::ams_sink_count() ==
          static_cast<std::size_t>(grown));

    ams.clear_backends();
    REQUIRE(FilamentConsumptionTrackerTestAccess::ams_sink_count() == 0);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "Tracker: mid-print register_sink auto-snapshots and tracks deltas",
                 "[consumption_sink][ams][registration]") {
    auto& ams = helix::AmsState::instance();
    auto& printer = get_printer_state();
    auto& tracker = FilamentConsumptionTracker::instance();

    ams.clear_backends();
    ams.deinit_subjects();
    ams.init_subjects(false);
    printer.init_subjects(false);
    ams.clear_external_spool_info();

    // Backend gets added pre-start so its per-slot sinks are registered while
    // no print is in progress. Seed slot 0 with a trackable configuration.
    auto m = std::make_unique<helix::AmsBackendMock>(4);
    helix::AmsBackendMock* mock = m.get();
    int backend_idx = ams.add_backend(std::move(m));
    helix::SlotInfo seed = mock->get_slot_info(0);
    seed.material = "PLA";
    seed.remaining_weight_g = 500.0f;
    seed.total_weight_g = 1000.0f;
    seed.spoolman_id = 0;
    mock->sync_external_identity(0, seed);

    // Start tracker and drive the printer into PRINTING so print_in_progress_
    // is true when we register a new sink below.
    tracker.start();
    lv_subject_set_int(printer.print_state().get_print_filament_used_subject(), 0);
    lv_subject_set_int(printer.print_state().get_print_state_enum_subject(),
                       static_cast<int>(helix::PrintJobState::PRINTING));
    helix::ui::UpdateQueue::instance().drain();

    // Register a fresh sink mid-print. Tracker should snapshot it immediately.
    helix::SlotInfo seed1 = mock->get_slot_info(1);
    seed1.material = "PLA";
    seed1.remaining_weight_g = 800.0f;
    seed1.total_weight_g = 1000.0f;
    seed1.spoolman_id = 0;
    mock->sync_external_identity(1, seed1);

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
    lv_subject_set_int(printer.print_state().get_print_filament_used_subject(), 1000);
    helix::ui::UpdateQueue::instance().drain();

    helix::SlotInfo after = mock->get_slot_info(1);
    // 1000mm PLA @ 1.75mm / 1.24 g/cm^3 ≈ 2.98 g.
    REQUIRE(after.remaining_weight_g < 800.0f);
    REQUIRE(after.remaining_weight_g > 796.0f);

    // Clean up.
    lv_subject_set_int(printer.print_state().get_print_state_enum_subject(),
                       static_cast<int>(helix::PrintJobState::COMPLETE));
    helix::ui::UpdateQueue::instance().drain();
    tracker.unregister_sink(handle);
    tracker.stop();
    ams.clear_backends();
    ams.clear_external_spool_info();
}

namespace {
// Records the order clear_backends() reaches each teardown step, and whether
// the backend was still registered when its sink's final flush persisted.
class TeardownOrderBackend : public helix::AmsBackendMock {
  public:
    TeardownOrderBackend(int slots, std::vector<std::string>& log)
        : helix::AmsBackendMock(slots), log_(log) {}
    void persist_slot_weight(int slot_index, float, float) override {
        const bool registered = helix::AmsState::instance().get_backend(0) == this;
        log_.push_back("flush slot " + std::to_string(slot_index) +
                       (registered ? " registered" : " unregistered"));
    }
    void stop() override {
        log_.push_back(helix::AmsState::instance().get_backend(0) == this ? "stop registered"
                                                                          : "stop unregistered");
        helix::AmsBackendMock::stop();
    }

  private:
    std::vector<std::string>& log_;
};
} // namespace

TEST_CASE_METHOD(
    LVGLTestFixture,
    "clear_backends flushes slot sinks while the backend is registered and stops it after",
    "[consumption_sink][ams][tracker_registry]") {
    auto& ams = helix::AmsState::instance();
    auto& printer = get_printer_state();
    auto& tracker = FilamentConsumptionTracker::instance();

    ams.clear_backends();
    ams.deinit_subjects();
    printer.init_subjects(false);
    ams.init_subjects(false);
    ams.clear_backends();

    std::vector<std::string> log;
    auto m = std::make_unique<TeardownOrderBackend>(4, log);
    TeardownOrderBackend* mock = m.get();
    REQUIRE(ams.add_backend(std::move(m)) == 0);
    helix::SlotInfo seed = mock->get_slot_info(0);
    seed.material = "PLA";
    seed.remaining_weight_g = 500.0f;
    seed.total_weight_g = 1000.0f;
    seed.spoolman_id = 0;
    mock->sync_external_identity(0, seed);

    // PRINTING snapshots every sink; only slot 0 is trackable, so only its
    // flush persists.
    tracker.start();
    lv_subject_set_int(printer.print_state().get_print_filament_used_subject(), 0);
    lv_subject_set_int(printer.print_state().get_print_state_enum_subject(),
                       static_cast<int>(helix::PrintJobState::PRINTING));
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(log.empty());

    ams.clear_backends();

    CHECK(log == std::vector<std::string>{"flush slot 0 registered", "stop unregistered"});
    CHECK(ams.get_backend(0) == nullptr);

    lv_subject_set_int(printer.print_state().get_print_state_enum_subject(),
                       static_cast<int>(helix::PrintJobState::STANDBY));
    helix::ui::UpdateQueue::instance().drain();
    tracker.stop();
    ams.deinit_subjects();
}

// ---------------------------------------------------------------------------
// A slot mapped to an extruder Klipper does not report never receives a delta,
// so it silently stops tracking. The tracker names such mappings at print
// start instead (prestonbrown/helixscreen#1373).
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(AmsSlotSinkFixture,
                 "tracker reports slot mappings past the printer's extruder count",
                 "[filament][tracker][ams_slot]") {
    auto& tracker = FilamentConsumptionTracker::instance();
    auto& printer = get_printer_state();

    SECTION("identity mapping on a single-extruder printer: three dead slots") {
        mock->set_identity_extruder_mapping_for_testing(true);
        printer.temperature_state().init_extruders({"extruder"});
        CHECK(FilamentConsumptionTrackerTestAccess::warn_unreported_extruder_mappings(tracker) ==
              3);
    }
    SECTION("every mapped extruder reported: nothing to say") {
        mock->set_identity_extruder_mapping_for_testing(true);
        printer.temperature_state().init_extruders(
            {"extruder", "extruder1", "extruder2", "extruder3"});
        CHECK(FilamentConsumptionTrackerTestAccess::warn_unreported_extruder_mappings(tracker) ==
              0);
    }
    SECTION("no mapping declared: nothing to say") {
        printer.temperature_state().init_extruders({"extruder"});
        CHECK(FilamentConsumptionTrackerTestAccess::warn_unreported_extruder_mappings(tracker) ==
              0);
    }
    SECTION("before extruder discovery the count is unknown, not zero") {
        mock->set_identity_extruder_mapping_for_testing(true);
        printer.temperature_state().init_extruders({});
        CHECK(FilamentConsumptionTrackerTestAccess::warn_unreported_extruder_mappings(tracker) ==
              0);
    }
}

TEST_CASE_METHOD(AmsSlotSinkFixture, "the PRINTING transition is what reports dead slot mappings",
                 "[filament][tracker][ams_slot]") {
    auto& tracker = FilamentConsumptionTracker::instance();
    auto& printer = get_printer_state();

    mock->set_identity_extruder_mapping_for_testing(true);
    printer.temperature_state().init_extruders({"extruder"});

    tracker.start();
    lv_subject_set_int(printer.print_state().get_print_filament_used_subject(), 0);
    lv_subject_set_int(printer.print_state().get_print_state_enum_subject(),
                       static_cast<int>(helix::PrintJobState::STANDBY));
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(FilamentConsumptionTrackerTestAccess::unreported_mappings_at_start(tracker) == 0);

    lv_subject_set_int(printer.print_state().get_print_state_enum_subject(),
                       static_cast<int>(helix::PrintJobState::PRINTING));
    helix::ui::UpdateQueue::instance().drain();
    CHECK(FilamentConsumptionTrackerTestAccess::unreported_mappings_at_start(tracker) == 3);

    lv_subject_set_int(printer.print_state().get_print_state_enum_subject(),
                       static_cast<int>(helix::PrintJobState::COMPLETE));
    helix::ui::UpdateQueue::instance().drain();
    tracker.stop();
}
