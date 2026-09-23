// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "ams_state.h"
#include "app_constants.h"
#include "app_globals.h"
#include "config.h"
#include "filament_consumption_tracker.h"
#include "filament_consumption_tracker_test_access.h"
#include "filament_database.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "test_helpers/unique_temp_dir.h"

#include <cstdlib>
#include <filesystem>

#include "../catch_amalgamated.hpp"

using Catch::Approx;
using Catch::Matchers::WithinAbs;
using namespace helix;

TEST_CASE("length_to_weight_g: 1.75mm PLA", "[filament][conversion]") {
    // 1.75mm PLA at 1.24 g/cm^3 is the canonical ~2.98 g/m.
    float grams = filament::length_to_weight_g(1000.0f, 1.24f, 1.75f);
    REQUIRE_THAT(grams, WithinAbs(2.982f, 0.01f));
}

TEST_CASE("length_to_weight_g: 2.85mm PLA", "[filament][conversion]") {
    float grams = filament::length_to_weight_g(1000.0f, 1.24f, 2.85f);
    // 2.85mm cross-section is (2.85/1.75)^2 ≈ 2.65x bigger.
    REQUIRE_THAT(grams, WithinAbs(7.91f, 0.05f));
}

TEST_CASE("length_to_weight_g: zero length", "[filament][conversion]") {
    REQUIRE(filament::length_to_weight_g(0.0f, 1.24f, 1.75f) == 0.0f);
}

TEST_CASE("length_to_weight_g: zero density returns zero", "[filament][conversion]") {
    // Callers must pre-check density; the function returns 0 as a safe default
    // instead of propagating NaN/Inf.
    REQUIRE(filament::length_to_weight_g(100.0f, 0.0f, 1.75f) == 0.0f);
}

TEST_CASE("set_external_spool_info_in_memory does not write settings", "[filament][ams_state]") {
    // Set up an isolated config directory so settings writes don't leak.
    std::string temp_dir = helix::test::unique_temp_dir("helix_fct_test");
    std::filesystem::create_directories(temp_dir);
    std::filesystem::remove(AppConstants::Update::config_backup_fallback());
    std::filesystem::remove(AppConstants::Update::legacy_config_backup_fallback());
    std::filesystem::remove(AppConstants::Update::env_backup_fallback());
    Config::get_instance()->init(temp_dir + "/settings.json");
    SettingsManager::instance().clear_external_spool_info();

    LVGLTestFixture fx;
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    auto& settings = SettingsManager::instance();

    // Baseline: clear the external spool.
    ams.clear_external_spool_info();

    SlotInfo info;
    info.material = "PLA";
    info.remaining_weight_g = 750.0f;
    info.total_weight_g = 1000.0f;
    info.color_rgb = 0xFF0000;

    // In-memory write: subject fires, but settings.json record stays absent.
    ams.set_external_spool_info_in_memory(info);

    auto persisted = settings.get_external_spool_info();
    REQUIRE_FALSE(persisted.has_value());

    // Persistent write: now it lands in settings.
    ams.set_external_spool_info(info);
    persisted = settings.get_external_spool_info();
    REQUIRE(persisted.has_value());
    REQUIRE(persisted->remaining_weight_g == 750.0f);

    ams.clear_external_spool_info();
    // Clear singleton state set by Config::init() above so the next test
    // doesn't see a stale path/active_printer_id_ pointing at a temp dir
    // that's about to vanish (tour-test wizard_completed read regression).
    Config::get_instance()->clear_path();
    std::filesystem::remove_all(temp_dir);
}

TEST_CASE("FilamentConsumptionTracker singleton exists", "[filament][tracker]") {
    auto& tracker = FilamentConsumptionTracker::instance();
    REQUIRE_FALSE(tracker.is_active());
}

TEST_CASE("tracker snapshots on transition to PRINTING with a valid PLA spool",
          "[filament][tracker]") {
    LVGLTestFixture fx;
    auto& ams = AmsState::instance();
    auto& printer = get_printer_state();
    auto& tracker = FilamentConsumptionTracker::instance();

    ams.init_subjects(false);
    printer.init_subjects(false);

    SlotInfo info;
    info.material = "PLA";
    info.remaining_weight_g = 1000.0f;
    info.total_weight_g = 1000.0f;
    ams.set_external_spool_info_in_memory(info);

    tracker.start();

    // Prime: printer starts in STANDBY, filament_used at 0.
    lv_subject_set_int(printer.get_print_state_enum_subject(),
                       static_cast<int>(PrintJobState::STANDBY));
    lv_subject_set_int(printer.get_print_filament_used_subject(), 0);
    REQUIRE_FALSE(tracker.is_active());

    // Transition to PRINTING: tracker should snapshot and activate.
    lv_subject_set_int(printer.get_print_state_enum_subject(),
                       static_cast<int>(PrintJobState::PRINTING));

    // Flush deferred queue so the observer callback runs.
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(tracker.is_active());

    tracker.stop();
    ams.clear_external_spool_info();
}

TEST_CASE("tracker stays inactive with no external spool", "[filament][tracker]") {
    LVGLTestFixture fx;
    auto& ams = AmsState::instance();
    auto& printer = get_printer_state();
    auto& tracker = FilamentConsumptionTracker::instance();

    ams.init_subjects(false);
    printer.init_subjects(false);

    ams.clear_external_spool_info();
    tracker.start();

    lv_subject_set_int(printer.get_print_state_enum_subject(),
                       static_cast<int>(PrintJobState::PRINTING));
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE_FALSE(tracker.is_active());
    tracker.stop();
}

TEST_CASE("tracker stays inactive when material cannot be resolved", "[filament][tracker]") {
    LVGLTestFixture fx;
    auto& ams = AmsState::instance();
    auto& printer = get_printer_state();
    auto& tracker = FilamentConsumptionTracker::instance();

    ams.init_subjects(false);
    printer.init_subjects(false);

    SlotInfo info;
    info.material = "UnknownNovelMaterial9000";
    info.remaining_weight_g = 1000.0f;
    info.total_weight_g = 1000.0f;
    ams.set_external_spool_info_in_memory(info);

    tracker.start();
    lv_subject_set_int(printer.get_print_state_enum_subject(),
                       static_cast<int>(PrintJobState::PRINTING));
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE_FALSE(tracker.is_active());
    tracker.stop();
    ams.clear_external_spool_info();
}

TEST_CASE("tracker decrements remaining weight as filament_used grows", "[filament][tracker]") {
    LVGLTestFixture fx;
    auto& ams = AmsState::instance();
    auto& printer = get_printer_state();
    auto& tracker = FilamentConsumptionTracker::instance();

    ams.init_subjects(false);
    printer.init_subjects(false);

    SlotInfo info;
    info.material = "PLA";
    info.remaining_weight_g = 1000.0f;
    info.total_weight_g = 1000.0f;
    ams.set_external_spool_info_in_memory(info);

    tracker.start();
    lv_subject_set_int(printer.get_print_filament_used_subject(), 0);
    lv_subject_set_int(printer.get_print_state_enum_subject(),
                       static_cast<int>(PrintJobState::PRINTING));
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(tracker.is_active());

    // Consume 1000 mm of 1.75mm PLA at 1.24 g/cm^3 ≈ 2.982 g.
    lv_subject_set_int(printer.get_print_filament_used_subject(), 1000);
    helix::ui::UpdateQueue::instance().drain();

    auto after = ams.get_external_spool_info();
    REQUIRE(after.has_value());
    REQUIRE(after->remaining_weight_g == Approx(997.018f).margin(0.05));

    tracker.stop();
    ams.clear_external_spool_info();
}

TEST_CASE("tracker clamps remaining weight at zero", "[filament][tracker]") {
    LVGLTestFixture fx;
    auto& ams = AmsState::instance();
    auto& printer = get_printer_state();
    auto& tracker = FilamentConsumptionTracker::instance();

    ams.init_subjects(false);
    printer.init_subjects(false);

    SlotInfo info;
    info.material = "PLA";
    info.remaining_weight_g = 5.0f; // only 5g available
    info.total_weight_g = 1000.0f;
    ams.set_external_spool_info_in_memory(info);

    tracker.start();
    lv_subject_set_int(printer.get_print_filament_used_subject(), 0);
    lv_subject_set_int(printer.get_print_state_enum_subject(),
                       static_cast<int>(PrintJobState::PRINTING));
    helix::ui::UpdateQueue::instance().drain();

    // Consume 10000mm = ~29.8g — would drive remaining negative without clamp.
    lv_subject_set_int(printer.get_print_filament_used_subject(), 10000);
    helix::ui::UpdateQueue::instance().drain();

    auto after = ams.get_external_spool_info();
    REQUIRE(after.has_value());
    REQUIRE(after->remaining_weight_g == 0.0f);

    tracker.stop();
    ams.clear_external_spool_info();
}

TEST_CASE("tracker persists final weight on print completion", "[filament][tracker]") {
    LVGLTestFixture fx;
    auto& ams = AmsState::instance();
    auto& printer = get_printer_state();
    auto& settings = SettingsManager::instance();
    auto& tracker = FilamentConsumptionTracker::instance();

    ams.init_subjects(false);
    printer.init_subjects(false);

    SlotInfo info;
    info.material = "PLA";
    info.remaining_weight_g = 1000.0f;
    info.total_weight_g = 1000.0f;
    ams.set_external_spool_info(info); // Start by persisting the initial value.

    tracker.start();
    lv_subject_set_int(printer.get_print_filament_used_subject(), 0);
    lv_subject_set_int(printer.get_print_state_enum_subject(),
                       static_cast<int>(PrintJobState::PRINTING));
    helix::ui::UpdateQueue::instance().drain();

    // Consume 1000 mm of 1.75mm PLA at 1.24 g/cm^3 ≈ 2.982 g.
    lv_subject_set_int(printer.get_print_filament_used_subject(), 1000);
    helix::ui::UpdateQueue::instance().drain();

    // Finish the print.
    lv_subject_set_int(printer.get_print_state_enum_subject(),
                       static_cast<int>(PrintJobState::COMPLETE));
    helix::ui::UpdateQueue::instance().drain();

    auto persisted = settings.get_external_spool_info();
    REQUIRE(persisted.has_value());
    REQUIRE(persisted->remaining_weight_g == Catch::Approx(997.018f).margin(0.05));
    REQUIRE_FALSE(tracker.is_active());

    tracker.stop();
    ams.clear_external_spool_info();
}

TEST_CASE("external weight edit during print triggers re-snapshot", "[filament][tracker]") {
    LVGLTestFixture fx;
    auto& ams = AmsState::instance();
    auto& printer = get_printer_state();
    auto& tracker = FilamentConsumptionTracker::instance();

    ams.init_subjects(false);
    printer.init_subjects(false);

    SlotInfo info;
    info.material = "PLA";
    info.remaining_weight_g = 1000.0f;
    info.total_weight_g = 1000.0f;
    ams.set_external_spool_info_in_memory(info);

    tracker.start();
    lv_subject_set_int(printer.get_print_filament_used_subject(), 0);
    lv_subject_set_int(printer.get_print_state_enum_subject(),
                       static_cast<int>(PrintJobState::PRINTING));
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(tracker.is_active());

    // Consume ~3g.
    lv_subject_set_int(printer.get_print_filament_used_subject(), 1000);
    helix::ui::UpdateQueue::instance().drain();
    auto snap1 = ams.get_external_spool_info();
    REQUIRE(snap1->remaining_weight_g == Catch::Approx(997.018f).margin(0.05));

    // Simulate an external writer (user edit, future Spoolman sync, etc.)
    // replacing the spool weight.
    SlotInfo edited = *snap1;
    edited.remaining_weight_g = 500.0f;
    ams.set_external_spool_info_in_memory(edited);

    // The 1100mm tick detects the external write and re-snapshots (returns early).
    // The 1200mm tick then computes delta = (1200 - 1100) * ~0.00298 ≈ 0.298g.
    // Without re-snapshot we'd see 500 - full_1200mm_delta ≈ 496.4g (broken).
    // With re-snapshot we see 500 - 0.298 ≈ 499.7g.
    lv_subject_set_int(printer.get_print_filament_used_subject(), 1100);
    helix::ui::UpdateQueue::instance().drain();
    lv_subject_set_int(printer.get_print_filament_used_subject(), 1200);
    helix::ui::UpdateQueue::instance().drain();

    auto snap2 = ams.get_external_spool_info();
    REQUIRE(snap2->remaining_weight_g == Catch::Approx(499.7f).margin(0.1));

    tracker.stop();
    ams.clear_external_spool_info();
}

TEST_CASE("tracker throttles disk persist during print", "[filament][tracker]") {
    LVGLTestFixture fx;
    auto& ams = AmsState::instance();
    auto& printer = get_printer_state();
    auto& settings = SettingsManager::instance();
    auto& tracker = FilamentConsumptionTracker::instance();

    ams.init_subjects(false);
    printer.init_subjects(false);

    SlotInfo info;
    info.material = "PLA";
    info.remaining_weight_g = 1000.0f;
    info.total_weight_g = 1000.0f;
    ams.set_external_spool_info(info);

    // start() installs the ExternalSpoolSink on its first call in the process,
    // so the throttle override has to come after it — otherwise the registry is
    // empty, the override silently misses, and the test measures the production
    // 60s interval (no flush, weight stays 1000g).
    tracker.start();
    REQUIRE(FilamentConsumptionTrackerTestAccess::set_persist_interval(tracker,
                                                                       1)); // every tick
    lv_subject_set_int(printer.get_print_filament_used_subject(), 0);
    lv_subject_set_int(printer.get_print_state_enum_subject(),
                       static_cast<int>(PrintJobState::PRINTING));
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(tracker.is_active());

    // Advance the tick so lv_tick_elaps() exceeds the 1ms test interval.
    lv_tick_inc(2);

    // Consume ~29.8g. With interval=1ms and 2ms elapsed, this should flush to disk.
    lv_subject_set_int(printer.get_print_filament_used_subject(), 10000);
    helix::ui::UpdateQueue::instance().drain();
    auto persisted = settings.get_external_spool_info();
    REQUIRE(persisted.has_value());
    REQUIRE(persisted->remaining_weight_g == Catch::Approx(970.18f).margin(0.1));

    // Restore the production interval for whatever test runs next — the sink is
    // a process-lifetime singleton.
    (void)FilamentConsumptionTrackerTestAccess::set_persist_interval(tracker, 0);
    tracker.stop();
    ams.clear_external_spool_info();
}
