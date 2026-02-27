// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_printer_print_char.cpp
 * @brief Characterization tests for PrinterState print domain
 *
 * These tests capture the CURRENT behavior of print-related subjects
 * in PrinterState before extraction to a dedicated PrinterPrintState class.
 *
 * Print subjects (17 total):
 *
 * Core State (4):
 * - print_state_ (string): "standby", "printing", "paused", "complete", "cancelled", "error"
 * - print_state_enum_ (int): PrintJobState enum (0-5)
 * - print_active_ (int): 1 when PRINTING or PAUSED, 0 otherwise
 * - print_outcome_ (int): PrintOutcome enum - persists through STANDBY transition
 *
 * File Info (3):
 * - print_filename_ (string): from print_stats.filename
 * - print_display_filename_ (string): set via API, cleaned name
 * - print_thumbnail_path_ (string): set via API, LVGL path
 *
 * Progress (2):
 * - print_progress_ (int 0-100): from virtual_sdcard.progress (float 0.0-1.0)
 * - print_show_progress_ (int): derived = print_active && print_start_phase == IDLE
 *
 * Layer Tracking (2):
 * - print_layer_current_ (int): from print_stats.info.current_layer
 * - print_layer_total_ (int): from print_stats.info.total_layer OR set via API
 *
 * Time Tracking (3):
 * - print_duration_ (int seconds): from print_stats.print_duration (extrusion only)
 * - print_elapsed_ (int seconds): from print_stats.total_duration (wall-clock elapsed)
 * - print_time_left_ (int seconds): estimated from print_duration and progress
 *
 * Print Start Phases (3):
 * - print_start_phase_ (int): PrintStartPhase enum (0-10)
 * - print_start_message_ (string): human-readable phase description
 * - print_start_progress_ (int 0-100): progress through PRINT_START
 *
 * Workflow (2):
 * - print_in_progress_ (int): 1 when G-code workflow running
 * - can_start_new_print(): returns false when print_in_progress_ == 1 OR print_active_ == 1
 */

#include "../test_helpers/printer_state_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;
using json = nlohmann::json;

// ============================================================================
// Initial State Tests - Document non-obvious default initialization
// ============================================================================

TEST_CASE("Print characterization: non-obvious initial values after init",
          "[characterization][print][init]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("print_state initializes to 'standby'") {
        const char* val = lv_subject_get_string(state.get_print_state_subject());
        REQUIRE(std::string(val) == "standby");
    }

    SECTION("print_state_enum initializes to STANDBY (0)") {
        REQUIRE(lv_subject_get_int(state.get_print_state_enum_subject()) ==
                static_cast<int>(PrintJobState::STANDBY));
    }

    SECTION("print_outcome initializes to NONE (0)") {
        REQUIRE(lv_subject_get_int(state.get_print_outcome_subject()) ==
                static_cast<int>(PrintOutcome::NONE));
    }

    SECTION("print_start_phase initializes to IDLE (0)") {
        REQUIRE(lv_subject_get_int(state.get_print_start_phase_subject()) ==
                static_cast<int>(PrintStartPhase::IDLE));
    }
}

// ============================================================================
// Core State JSON Update Tests
// ============================================================================

TEST_CASE("Print characterization: core state from JSON", "[characterization][print][update]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("standby state updates correctly") {
        json status = {{"print_stats", {{"state", "standby"}}}};
        state.update_from_status(status);

        REQUIRE(std::string(lv_subject_get_string(state.get_print_state_subject())) == "standby");
        REQUIRE(lv_subject_get_int(state.get_print_state_enum_subject()) ==
                static_cast<int>(PrintJobState::STANDBY));
        REQUIRE(lv_subject_get_int(state.get_print_active_subject()) == 0);
    }

    SECTION("printing state updates correctly") {
        json status = {{"print_stats", {{"state", "printing"}}}};
        state.update_from_status(status);

        REQUIRE(std::string(lv_subject_get_string(state.get_print_state_subject())) == "printing");
        REQUIRE(lv_subject_get_int(state.get_print_state_enum_subject()) ==
                static_cast<int>(PrintJobState::PRINTING));
        REQUIRE(lv_subject_get_int(state.get_print_active_subject()) == 1);
    }

    SECTION("paused state updates correctly") {
        json status = {{"print_stats", {{"state", "paused"}}}};
        state.update_from_status(status);

        REQUIRE(std::string(lv_subject_get_string(state.get_print_state_subject())) == "paused");
        REQUIRE(lv_subject_get_int(state.get_print_state_enum_subject()) ==
                static_cast<int>(PrintJobState::PAUSED));
        REQUIRE(lv_subject_get_int(state.get_print_active_subject()) == 1);
    }

    SECTION("complete state updates correctly") {
        json status = {{"print_stats", {{"state", "complete"}}}};
        state.update_from_status(status);

        REQUIRE(std::string(lv_subject_get_string(state.get_print_state_subject())) == "complete");
        REQUIRE(lv_subject_get_int(state.get_print_state_enum_subject()) ==
                static_cast<int>(PrintJobState::COMPLETE));
        REQUIRE(lv_subject_get_int(state.get_print_active_subject()) == 0);
    }

    SECTION("cancelled state updates correctly") {
        json status = {{"print_stats", {{"state", "cancelled"}}}};
        state.update_from_status(status);

        REQUIRE(std::string(lv_subject_get_string(state.get_print_state_subject())) == "cancelled");
        REQUIRE(lv_subject_get_int(state.get_print_state_enum_subject()) ==
                static_cast<int>(PrintJobState::CANCELLED));
        REQUIRE(lv_subject_get_int(state.get_print_active_subject()) == 0);
    }

    SECTION("error state updates correctly") {
        json status = {{"print_stats", {{"state", "error"}}}};
        state.update_from_status(status);

        REQUIRE(std::string(lv_subject_get_string(state.get_print_state_subject())) == "error");
        REQUIRE(lv_subject_get_int(state.get_print_state_enum_subject()) ==
                static_cast<int>(PrintJobState::ERROR));
        REQUIRE(lv_subject_get_int(state.get_print_active_subject()) == 0);
    }

    SECTION("unknown state defaults to standby") {
        json status = {{"print_stats", {{"state", "unknown_state"}}}};
        state.update_from_status(status);

        // String subject gets the raw value
        REQUIRE(std::string(lv_subject_get_string(state.get_print_state_subject())) ==
                "unknown_state");
        // Enum defaults to STANDBY for unknown strings
        REQUIRE(lv_subject_get_int(state.get_print_state_enum_subject()) ==
                static_cast<int>(PrintJobState::STANDBY));
    }
}

// ============================================================================
// Terminal State Persistence Tests (print_outcome)
// ============================================================================

TEST_CASE("Print characterization: terminal state persistence",
          "[characterization][print][outcome]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("COMPLETE sets outcome to COMPLETE") {
        // Start with printing
        json printing = {{"print_stats", {{"state", "printing"}}}};
        state.update_from_status(printing);
        REQUIRE(lv_subject_get_int(state.get_print_outcome_subject()) ==
                static_cast<int>(PrintOutcome::NONE));

        // Complete the print
        json complete = {{"print_stats", {{"state", "complete"}}}};
        state.update_from_status(complete);
        REQUIRE(lv_subject_get_int(state.get_print_outcome_subject()) ==
                static_cast<int>(PrintOutcome::COMPLETE));
    }

    SECTION("CANCELLED sets outcome to CANCELLED") {
        json printing = {{"print_stats", {{"state", "printing"}}}};
        state.update_from_status(printing);

        json cancelled = {{"print_stats", {{"state", "cancelled"}}}};
        state.update_from_status(cancelled);
        REQUIRE(lv_subject_get_int(state.get_print_outcome_subject()) ==
                static_cast<int>(PrintOutcome::CANCELLED));
    }

    SECTION("ERROR sets outcome to ERROR") {
        json printing = {{"print_stats", {{"state", "printing"}}}};
        state.update_from_status(printing);

        json error = {{"print_stats", {{"state", "error"}}}};
        state.update_from_status(error);
        REQUIRE(lv_subject_get_int(state.get_print_outcome_subject()) ==
                static_cast<int>(PrintOutcome::ERROR));
    }

    SECTION("outcome persists through transition to STANDBY") {
        // Complete a print
        json printing = {{"print_stats", {{"state", "printing"}}}};
        state.update_from_status(printing);

        json complete = {{"print_stats", {{"state", "complete"}}}};
        state.update_from_status(complete);
        REQUIRE(lv_subject_get_int(state.get_print_outcome_subject()) ==
                static_cast<int>(PrintOutcome::COMPLETE));

        // Transition to standby (Moonraker does this after print completion)
        json standby = {{"print_stats", {{"state", "standby"}}}};
        state.update_from_status(standby);

        // Outcome should PERSIST (not reset to NONE)
        REQUIRE(lv_subject_get_int(state.get_print_outcome_subject()) ==
                static_cast<int>(PrintOutcome::COMPLETE));
    }

    SECTION("outcome clears when NEW print starts (PRINTING from non-PAUSED)") {
        // Complete a print first
        json printing = {{"print_stats", {{"state", "printing"}}}};
        state.update_from_status(printing);

        json complete = {{"print_stats", {{"state", "complete"}}}};
        state.update_from_status(complete);
        REQUIRE(lv_subject_get_int(state.get_print_outcome_subject()) ==
                static_cast<int>(PrintOutcome::COMPLETE));

        // Go to standby
        json standby = {{"print_stats", {{"state", "standby"}}}};
        state.update_from_status(standby);

        // Start a NEW print (STANDBY -> PRINTING)
        json new_print = {{"print_stats", {{"state", "printing"}}}};
        state.update_from_status(new_print);

        // Outcome should be cleared
        REQUIRE(lv_subject_get_int(state.get_print_outcome_subject()) ==
                static_cast<int>(PrintOutcome::NONE));
    }

    SECTION("resume from PAUSED keeps outcome (does not clear)") {
        // Start printing
        json printing = {{"print_stats", {{"state", "printing"}}}};
        state.update_from_status(printing);

        // Pause
        json paused = {{"print_stats", {{"state", "paused"}}}};
        state.update_from_status(paused);
        REQUIRE(lv_subject_get_int(state.get_print_outcome_subject()) ==
                static_cast<int>(PrintOutcome::NONE));

        // Resume (PAUSED -> PRINTING)
        state.update_from_status(printing);

        // Outcome should remain NONE (not cleared, just not set)
        REQUIRE(lv_subject_get_int(state.get_print_outcome_subject()) ==
                static_cast<int>(PrintOutcome::NONE));
    }

    SECTION("set_print_outcome API works") {
        state.set_print_outcome(PrintOutcome::CANCELLED);
        REQUIRE(lv_subject_get_int(state.get_print_outcome_subject()) ==
                static_cast<int>(PrintOutcome::CANCELLED));
    }
}

// ============================================================================
// File Info Tests
// ============================================================================

TEST_CASE("Print characterization: file info from JSON", "[characterization][print][file]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("filename updates from print_stats.filename") {
        json status = {{"print_stats", {{"filename", "test_model.gcode"}}}};
        state.update_from_status(status);

        const char* val = lv_subject_get_string(state.get_print_filename_subject());
        REQUIRE(std::string(val) == "test_model.gcode");
    }

    SECTION("filename with path") {
        json status = {{"print_stats", {{"filename", "folder/subfolder/model.gcode"}}}};
        state.update_from_status(status);

        const char* val = lv_subject_get_string(state.get_print_filename_subject());
        REQUIRE(std::string(val) == "folder/subfolder/model.gcode");
    }

    SECTION("empty filename") {
        json status = {{"print_stats", {{"filename", ""}}}};
        state.update_from_status(status);

        const char* val = lv_subject_get_string(state.get_print_filename_subject());
        REQUIRE(std::string(val) == "");
    }
}

TEST_CASE("Print characterization: file info API methods", "[characterization][print][file][api]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("set_print_display_filename updates subject") {
        state.set_print_display_filename("Clean Model Name");

        const char* val = lv_subject_get_string(state.get_print_display_filename_subject());
        REQUIRE(std::string(val) == "Clean Model Name");
    }

    SECTION("set_print_thumbnail_path updates subject") {
        state.set_print_thumbnail_path("A:/tmp/thumbnail_abc123.bin");

        const char* val = lv_subject_get_string(state.get_print_thumbnail_path_subject());
        REQUIRE(std::string(val) == "A:/tmp/thumbnail_abc123.bin");
    }

    SECTION("empty thumbnail path clears subject") {
        state.set_print_thumbnail_path("A:/tmp/thumbnail.bin");
        state.set_print_thumbnail_path("");

        const char* val = lv_subject_get_string(state.get_print_thumbnail_path_subject());
        REQUIRE(std::string(val) == "");
    }
}

// ============================================================================
// Progress Tests
// ============================================================================

TEST_CASE("Print characterization: progress from JSON", "[characterization][print][progress]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("progress converts 0.0-1.0 float to 0-100 percentage") {
        json status = {{"virtual_sdcard", {{"progress", 0.5}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 50);
    }

    SECTION("progress 0.0 becomes 0%") {
        json status = {{"virtual_sdcard", {{"progress", 0.0}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 0);
    }

    SECTION("progress 1.0 becomes 100%") {
        json status = {{"virtual_sdcard", {{"progress", 1.0}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 100);
    }

    SECTION("progress 0.753 becomes 75%") {
        json status = {{"virtual_sdcard", {{"progress", 0.753}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 75);
    }
}

TEST_CASE("Print characterization: terminal state progress guard",
          "[characterization][print][progress][guard]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("progress cannot go backward in COMPLETE state") {
        // Set up complete state with 100% progress
        json printing = {{"print_stats", {{"state", "printing"}}},
                         {"virtual_sdcard", {{"progress", 1.0}}}};
        state.update_from_status(printing);
        REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 100);

        json complete = {{"print_stats", {{"state", "complete"}}}};
        state.update_from_status(complete);

        // Try to set progress to 0 (Moonraker does this sometimes)
        json reset = {{"virtual_sdcard", {{"progress", 0.0}}}};
        state.update_from_status(reset);

        // Progress should stay at 100 (guarded)
        REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 100);
    }

    SECTION("progress cannot go backward in CANCELLED state") {
        json printing = {{"print_stats", {{"state", "printing"}}},
                         {"virtual_sdcard", {{"progress", 0.75}}}};
        state.update_from_status(printing);
        REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 75);

        json cancelled = {{"print_stats", {{"state", "cancelled"}}}};
        state.update_from_status(cancelled);

        json reset = {{"virtual_sdcard", {{"progress", 0.0}}}};
        state.update_from_status(reset);

        // Progress should stay at 75 (guarded)
        REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 75);
    }

    SECTION("progress cannot go backward in ERROR state") {
        json printing = {{"print_stats", {{"state", "printing"}}},
                         {"virtual_sdcard", {{"progress", 0.5}}}};
        state.update_from_status(printing);

        json error = {{"print_stats", {{"state", "error"}}}};
        state.update_from_status(error);

        json reset = {{"virtual_sdcard", {{"progress", 0.0}}}};
        state.update_from_status(reset);

        // Progress should stay at 50 (guarded)
        REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 50);
    }

    SECTION("progress CAN go forward in terminal state") {
        json printing = {{"print_stats", {{"state", "printing"}}},
                         {"virtual_sdcard", {{"progress", 0.95}}}};
        state.update_from_status(printing);

        json complete = {{"print_stats", {{"state", "complete"}}}};
        state.update_from_status(complete);

        // Can still update to 100%
        json full = {{"virtual_sdcard", {{"progress", 1.0}}}};
        state.update_from_status(full);

        REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 100);
    }

    SECTION("progress can reset in non-terminal states") {
        json printing = {{"print_stats", {{"state", "printing"}}},
                         {"virtual_sdcard", {{"progress", 0.5}}}};
        state.update_from_status(printing);

        json standby = {{"print_stats", {{"state", "standby"}}}};
        state.update_from_status(standby);

        // In standby, progress CAN go backward
        json reset = {{"virtual_sdcard", {{"progress", 0.0}}}};
        state.update_from_status(reset);

        REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 0);
    }
}

// ============================================================================
// Layer Tracking Tests
// ============================================================================

TEST_CASE("Print characterization: layer tracking from JSON", "[characterization][print][layer]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("current_layer updates from print_stats.info") {
        json status = {{"print_stats", {{"info", {{"current_layer", 42}}}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 42);
    }

    SECTION("total_layer updates from print_stats.info") {
        json status = {{"print_stats", {{"info", {{"total_layer", 150}}}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.get_print_layer_total_subject()) == 150);
    }

    SECTION("both layers update together") {
        json status = {{"print_stats", {{"info", {{"current_layer", 25}, {"total_layer", 100}}}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 25);
        REQUIRE(lv_subject_get_int(state.get_print_layer_total_subject()) == 100);
    }

    SECTION("set_print_layer_total API updates subject") {
        state.set_print_layer_total(200);

        REQUIRE(lv_subject_get_int(state.get_print_layer_total_subject()) == 200);
    }

    SECTION("null layer values are ignored") {
        // Set initial value
        state.set_print_layer_total(100);

        // Moonraker sometimes sends null for layer info
        json status = {{"print_stats", {{"info", {{"current_layer", nullptr}}}}}};
        state.update_from_status(status);

        // Values should remain unchanged (null is not a number)
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 0);
        REQUIRE(lv_subject_get_int(state.get_print_layer_total_subject()) == 100);
    }
}

// ============================================================================
// Time Tracking Tests
// ============================================================================

TEST_CASE("Print characterization: time tracking from JSON", "[characterization][print][time]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("print_duration updates from print_stats.print_duration") {
        json status = {{"print_stats", {{"print_duration", 3600.0}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.get_print_duration_subject()) == 3600);
    }

    SECTION("print_elapsed updates from print_stats.total_duration") {
        json status = {{"print_stats", {{"total_duration", 360.0}}}};
        state.update_from_status(status);

        // total_duration = wall-clock elapsed since job started
        REQUIRE(lv_subject_get_int(state.get_print_elapsed_subject()) == 360);
    }

    SECTION("time_left estimated from progress and print_duration") {
        // Set progress to 50%
        json status1 = {{"virtual_sdcard", {{"progress", 0.5}}}};
        state.update_from_status(status1);

        // Set print_duration (actual print time) and total_duration (wall-clock)
        // Remaining estimate uses print_duration, not total_duration
        json status2 = {{"print_stats", {{"print_duration", 3600.0}, {"total_duration", 3600.0}}}};
        state.update_from_status(status2);

        // remaining = print_duration * (100 - progress) / progress
        // remaining = 3600 * (100 - 50) / 50 = 3600
        REQUIRE(lv_subject_get_int(state.get_print_time_left_subject()) == 3600);
    }

    SECTION("time_left zero when progress is 100%") {
        json status = {{"virtual_sdcard", {{"progress", 1.0}}},
                       {"print_stats", {{"print_duration", 7200.0}, {"total_duration", 7200.0}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.get_print_time_left_subject()) == 0);
    }

    SECTION("time_left estimated at low progress with extrapolation") {
        // At 3% progress with no slicer estimate, pure extrapolation is used
        // (blend only engages when estimated_print_time_ > 0)
        json status1 = {{"virtual_sdcard", {{"progress", 0.03}}}};
        state.update_from_status(status1);

        json status2 = {{"print_stats", {{"print_duration", 360.0}, {"total_duration", 400.0}}}};
        state.update_from_status(status2);

        // 360 * (100-3) / 3 = 11640
        REQUIRE(lv_subject_get_int(state.get_print_time_left_subject()) == 11640);
    }

    SECTION("time_left not updated when progress is 0") {
        // With no progress, remaining cannot be estimated
        json status = {{"print_stats", {{"print_duration", 360.0}, {"total_duration", 360.0}}}};
        state.update_from_status(status);

        // time_left stays at 0
        REQUIRE(lv_subject_get_int(state.get_print_time_left_subject()) == 0);
    }

    SECTION("time_left uses print_duration not total_duration (prep time excluded)") {
        // Simulate a print that spent significant time in prep:
        // 300s total wall-clock, but only 30s of actual printing at 7% progress
        json progress_status = {{"virtual_sdcard", {{"progress", 0.07}}}};
        state.update_from_status(progress_status);

        json status = {{"print_stats", {{"print_duration", 30.0}, {"total_duration", 300.0}}}};
        state.update_from_status(status);

        // Using print_duration: 30 * (100-7)/7 = 30 * 93/7 = 398
        // NOT total_duration:  300 * (100-7)/7 = 300 * 93/7 = 3985 (wildly wrong)
        int remaining = lv_subject_get_int(state.get_print_time_left_subject());
        REQUIRE(remaining == 398);
        REQUIRE(remaining < 500); // Sanity check: reasonable for a short print
    }

    SECTION("time_left not updated when print_duration is 0 (all prep time)") {
        // Progress is 5% but print_duration is 0 (Moonraker sometimes does this
        // at very early stages when only prep has happened)
        json progress_status = {{"virtual_sdcard", {{"progress", 0.05}}}};
        state.update_from_status(progress_status);

        json status = {{"print_stats", {{"print_duration", 0.0}, {"total_duration", 200.0}}}};
        state.update_from_status(status);

        // Should stay at 0 (can't estimate with no actual print time)
        REQUIRE(lv_subject_get_int(state.get_print_time_left_subject()) == 0);
    }

    SECTION("both duration and total in same update with progress") {
        // Set progress first
        json progress_status = {{"virtual_sdcard", {{"progress", 0.25}}}};
        state.update_from_status(progress_status);

        json status = {{"print_stats", {{"print_duration", 1800.0}, {"total_duration", 2000.0}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.get_print_duration_subject()) == 1800);
        REQUIRE(lv_subject_get_int(state.get_print_elapsed_subject()) == 2000);
        // remaining = print_duration * (100 - 25) / 25 = 1800 * 3 = 5400
        REQUIRE(lv_subject_get_int(state.get_print_time_left_subject()) == 5400);
    }
}

// ============================================================================
// Print Start Phase Tests
// ============================================================================

TEST_CASE("Print characterization: print start phases", "[characterization][print][phase]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("set_print_start_state updates all three subjects") {
        state.set_print_start_state(PrintStartPhase::HEATING_BED, "Heating bed...", 30);

        // Drain the async queue to apply the updates
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        REQUIRE(lv_subject_get_int(state.get_print_start_phase_subject()) ==
                static_cast<int>(PrintStartPhase::HEATING_BED));
        REQUIRE(std::string(lv_subject_get_string(state.get_print_start_message_subject())) ==
                "Heating bed...");
        REQUIRE(lv_subject_get_int(state.get_print_start_progress_subject()) == 30);
    }

    SECTION("is_in_print_start returns true when phase is not IDLE") {
        REQUIRE(state.is_in_print_start() == false);

        state.set_print_start_state(PrintStartPhase::HOMING, "Homing...", 10);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        REQUIRE(state.is_in_print_start() == true);
    }

    SECTION("reset_print_start_state sets phase to IDLE") {
        state.set_print_start_state(PrintStartPhase::QGL, "QGL...", 50);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        state.reset_print_start_state();
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        REQUIRE(lv_subject_get_int(state.get_print_start_phase_subject()) ==
                static_cast<int>(PrintStartPhase::IDLE));
        REQUIRE(std::string(lv_subject_get_string(state.get_print_start_message_subject())) == "");
        REQUIRE(lv_subject_get_int(state.get_print_start_progress_subject()) == 0);
    }

    SECTION("progress is clamped to 0-100") {
        state.set_print_start_state(PrintStartPhase::INITIALIZING, "Starting...", 150);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        REQUIRE(lv_subject_get_int(state.get_print_start_progress_subject()) == 100);

        state.set_print_start_state(PrintStartPhase::INITIALIZING, "Starting...", -10);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        REQUIRE(lv_subject_get_int(state.get_print_start_progress_subject()) == 0);
    }

    SECTION("all PrintStartPhase enum values are valid") {
        // Test that all enum values can be set
        std::vector<PrintStartPhase> phases = {
            PrintStartPhase::IDLE,           PrintStartPhase::INITIALIZING,
            PrintStartPhase::HOMING,         PrintStartPhase::HEATING_BED,
            PrintStartPhase::HEATING_NOZZLE, PrintStartPhase::QGL,
            PrintStartPhase::Z_TILT,         PrintStartPhase::BED_MESH,
            PrintStartPhase::CLEANING,       PrintStartPhase::PURGING,
            PrintStartPhase::COMPLETE};

        for (auto phase : phases) {
            state.set_print_start_state(phase, "Test", 50);
            UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

            REQUIRE(lv_subject_get_int(state.get_print_start_phase_subject()) ==
                    static_cast<int>(phase));
        }
    }
}

TEST_CASE("Print characterization: preparing phase clears outcome",
          "[characterization][print][outcome][phase]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("entering Preparing from COMPLETE clears outcome immediately") {
        // Simulate: print completes
        json complete = {{"print_stats", {{"state", "complete"}}}};
        state.update_from_status(complete);
        REQUIRE(lv_subject_get_int(state.get_print_outcome_subject()) ==
                static_cast<int>(PrintOutcome::COMPLETE));

        // User starts a new print — PRINT_START fires, entering Preparing phase.
        // Moonraker state is still "standby" at this point (not yet "printing").
        state.set_print_start_state(PrintStartPhase::HOMING, "Homing...", 10);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        // Outcome should be cleared immediately — cancel button visible during pre-print
        REQUIRE(lv_subject_get_int(state.get_print_outcome_subject()) ==
                static_cast<int>(PrintOutcome::NONE));
    }

    SECTION("entering Preparing from CANCELLED clears outcome immediately") {
        json cancelled = {{"print_stats", {{"state", "cancelled"}}}};
        state.update_from_status(cancelled);
        REQUIRE(lv_subject_get_int(state.get_print_outcome_subject()) ==
                static_cast<int>(PrintOutcome::CANCELLED));

        state.set_print_start_state(PrintStartPhase::HEATING_BED, "Heating bed...", 20);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        REQUIRE(lv_subject_get_int(state.get_print_outcome_subject()) ==
                static_cast<int>(PrintOutcome::NONE));
    }

    SECTION("entering Preparing from ERROR clears outcome immediately") {
        json error = {{"print_stats", {{"state", "error"}}}};
        state.update_from_status(error);
        REQUIRE(lv_subject_get_int(state.get_print_outcome_subject()) ==
                static_cast<int>(PrintOutcome::ERROR));

        state.set_print_start_state(PrintStartPhase::INITIALIZING, "Starting...", 5);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        REQUIRE(lv_subject_get_int(state.get_print_outcome_subject()) ==
                static_cast<int>(PrintOutcome::NONE));
    }

    SECTION("phase change within Preparing does NOT re-clear outcome") {
        // Simulate complete -> preparing
        json complete = {{"print_stats", {{"state", "complete"}}}};
        state.update_from_status(complete);

        state.set_print_start_state(PrintStartPhase::HOMING, "Homing...", 10);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
        REQUIRE(lv_subject_get_int(state.get_print_outcome_subject()) ==
                static_cast<int>(PrintOutcome::NONE));

        // Advance through phases — outcome stays NONE, no redundant clearing
        state.set_print_start_state(PrintStartPhase::HEATING_BED, "Heating bed...", 40);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
        REQUIRE(lv_subject_get_int(state.get_print_outcome_subject()) ==
                static_cast<int>(PrintOutcome::NONE));
    }
}

TEST_CASE("Print characterization: print start phase safety reset",
          "[characterization][print][phase][safety]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("phase resets to IDLE when print_active becomes 0") {
        // Start a print and enter a phase
        json printing = {{"print_stats", {{"state", "printing"}}}};
        state.update_from_status(printing);

        state.set_print_start_state(PrintStartPhase::HEATING_NOZZLE, "Heating...", 40);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        REQUIRE(lv_subject_get_int(state.get_print_start_phase_subject()) ==
                static_cast<int>(PrintStartPhase::HEATING_NOZZLE));

        // Print ends (goes to complete)
        json complete = {{"print_stats", {{"state", "complete"}}}};
        state.update_from_status(complete);

        // Phase should be reset to IDLE (safety mechanism)
        REQUIRE(lv_subject_get_int(state.get_print_start_phase_subject()) ==
                static_cast<int>(PrintStartPhase::IDLE));
    }
}

// ============================================================================
// print_show_progress Derived Subject Tests
// ============================================================================

TEST_CASE("Print characterization: print_show_progress derived subject",
          "[characterization][print][derived]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("print_show_progress is 0 when not printing") {
        REQUIRE(lv_subject_get_int(state.get_print_show_progress_subject()) == 0);
    }

    SECTION("print_show_progress is 0 during print start phase") {
        // Start printing but in a start phase
        json printing = {{"print_stats", {{"state", "printing"}}}};
        state.update_from_status(printing);

        state.set_print_start_state(PrintStartPhase::HEATING_BED, "Heating...", 30);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        // Active but in start phase = don't show progress yet
        REQUIRE(lv_subject_get_int(state.get_print_active_subject()) == 1);
        REQUIRE(lv_subject_get_int(state.get_print_show_progress_subject()) == 0);
    }

    SECTION("print_show_progress is 1 when printing and phase is IDLE") {
        json printing = {{"print_stats", {{"state", "printing"}}}};
        state.update_from_status(printing);

        // Phase should be IDLE by default
        REQUIRE(lv_subject_get_int(state.get_print_start_phase_subject()) ==
                static_cast<int>(PrintStartPhase::IDLE));
        REQUIRE(lv_subject_get_int(state.get_print_show_progress_subject()) == 1);
    }

    SECTION("print_show_progress is 1 when paused and phase is IDLE") {
        json paused = {{"print_stats", {{"state", "paused"}}}};
        state.update_from_status(paused);

        REQUIRE(lv_subject_get_int(state.get_print_active_subject()) == 1);
        REQUIRE(lv_subject_get_int(state.get_print_show_progress_subject()) == 1);
    }

    SECTION("print_show_progress becomes 1 when phase transitions to IDLE") {
        // Start printing in a phase
        json printing = {{"print_stats", {{"state", "printing"}}}};
        state.update_from_status(printing);

        state.set_print_start_state(PrintStartPhase::COMPLETE, "Done", 100);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
        REQUIRE(lv_subject_get_int(state.get_print_show_progress_subject()) == 0);

        // Phase goes to IDLE
        state.reset_print_start_state();
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        REQUIRE(lv_subject_get_int(state.get_print_show_progress_subject()) == 1);
    }
}

// ============================================================================
// Workflow Tests (print_in_progress and can_start_new_print)
// ============================================================================

TEST_CASE("Print characterization: workflow state", "[characterization][print][workflow]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("set_print_in_progress updates subject") {
        REQUIRE(lv_subject_get_int(state.get_print_in_progress_subject()) == 0);

        state.set_print_in_progress(true);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        REQUIRE(lv_subject_get_int(state.get_print_in_progress_subject()) == 1);

        state.set_print_in_progress(false);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        REQUIRE(lv_subject_get_int(state.get_print_in_progress_subject()) == 0);
    }

    SECTION("is_print_in_progress returns correct value") {
        REQUIRE(state.is_print_in_progress() == false);

        state.set_print_in_progress(true);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        REQUIRE(state.is_print_in_progress() == true);
    }

    SECTION("can_start_new_print returns true when idle and not in progress") {
        // Default state: standby, not in progress
        REQUIRE(state.can_start_new_print() == true);
    }

    SECTION("can_start_new_print returns false when print_in_progress is true") {
        state.set_print_in_progress(true);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        REQUIRE(state.can_start_new_print() == false);
    }

    SECTION("can_start_new_print returns false when PRINTING") {
        json printing = {{"print_stats", {{"state", "printing"}}}};
        state.update_from_status(printing);

        REQUIRE(state.can_start_new_print() == false);
    }

    SECTION("can_start_new_print returns false when PAUSED") {
        json paused = {{"print_stats", {{"state", "paused"}}}};
        state.update_from_status(paused);

        REQUIRE(state.can_start_new_print() == false);
    }

    SECTION("can_start_new_print returns true when COMPLETE") {
        json complete = {{"print_stats", {{"state", "complete"}}}};
        state.update_from_status(complete);

        REQUIRE(state.can_start_new_print() == true);
    }

    SECTION("can_start_new_print returns true when CANCELLED") {
        json cancelled = {{"print_stats", {{"state", "cancelled"}}}};
        state.update_from_status(cancelled);

        REQUIRE(state.can_start_new_print() == true);
    }

    SECTION("can_start_new_print returns true when ERROR") {
        json error = {{"print_stats", {{"state", "error"}}}};
        state.update_from_status(error);

        REQUIRE(state.can_start_new_print() == true);
    }

    SECTION("can_start_new_print returns true when STANDBY") {
        json standby = {{"print_stats", {{"state", "standby"}}}};
        state.update_from_status(standby);

        REQUIRE(state.can_start_new_print() == true);
    }
}

// ============================================================================
// reset_for_new_print Tests
// ============================================================================

TEST_CASE("Print characterization: reset_for_new_print clears progress subjects",
          "[characterization][print][reset]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Set various print values
    json status = {{"print_stats",
                    {{"filename", "test.gcode"},
                     {"print_duration", 3600.0},
                     {"info", {{"current_layer", 50}, {"total_layer", 100}}}}},
                   {"virtual_sdcard", {{"progress", 0.5}}}};
    state.update_from_status(status);

    REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 50);
    REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 50);
    REQUIRE(lv_subject_get_int(state.get_print_duration_subject()) == 3600);

    // Reset for new print
    state.reset_for_new_print();

    // These should be cleared
    REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 0);
    REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 0);
    REQUIRE(lv_subject_get_int(state.get_print_duration_subject()) == 0);
    REQUIRE(lv_subject_get_int(state.get_print_time_left_subject()) == 0);

    // Filename should NOT be cleared (it's Moonraker's source of truth)
    const char* filename = lv_subject_get_string(state.get_print_filename_subject());
    REQUIRE(std::string(filename) == "test.gcode");
}

// ============================================================================
// Observer Notification Tests
// ============================================================================

TEST_CASE("Print characterization: observer fires when print_state_enum changes",
          "[characterization][print][observer]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    auto observer_cb = [](lv_observer_t* observer, lv_subject_t* subject) {
        int* count_ptr = static_cast<int*>(lv_observer_get_user_data(observer));
        int* value_ptr = count_ptr + 1;

        (*count_ptr)++;
        *value_ptr = lv_subject_get_int(subject);
    };

    int user_data[2] = {0, -1}; // [callback_count, last_value]

    lv_observer_t* observer =
        lv_subject_add_observer(state.get_print_state_enum_subject(), observer_cb, user_data);

    // LVGL auto-notifies observers when first added
    REQUIRE(user_data[0] == 1);
    REQUIRE(user_data[1] == static_cast<int>(PrintJobState::STANDBY));

    // Change to printing
    json status = {{"print_stats", {{"state", "printing"}}}};
    state.update_from_status(status);

    REQUIRE(user_data[0] >= 2);
    REQUIRE(user_data[1] == static_cast<int>(PrintJobState::PRINTING));

    lv_observer_remove(observer);
}

TEST_CASE("Print characterization: observer fires when print_progress changes",
          "[characterization][print][observer]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    auto observer_cb = [](lv_observer_t* observer, lv_subject_t* subject) {
        int* count_ptr = static_cast<int*>(lv_observer_get_user_data(observer));
        int* value_ptr = count_ptr + 1;

        (*count_ptr)++;
        *value_ptr = lv_subject_get_int(subject);
    };

    int user_data[2] = {0, -1};

    lv_observer_t* observer =
        lv_subject_add_observer(state.get_print_progress_subject(), observer_cb, user_data);

    // Initial notification
    REQUIRE(user_data[0] == 1);
    REQUIRE(user_data[1] == 0);

    // Update progress
    json status = {{"virtual_sdcard", {{"progress", 0.75}}}};
    state.update_from_status(status);

    REQUIRE(user_data[0] >= 2);
    REQUIRE(user_data[1] == 75);

    lv_observer_remove(observer);
}

TEST_CASE("Print: string observer sees updated enum (issue #125 regression)",
          "[print][observer][regression]") {
    // Issue #125: PrintSelectPanel observes print_state_ (string) but reads
    // print_state_enum_ to decide if Print button should be enabled. If the
    // string subject fires before the enum is updated, can_start_new_print()
    // reads stale PRINTING state and keeps the button disabled permanently.
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Start a print
    json printing = {{"print_stats", {{"state", "printing"}}}};
    state.update_from_status(printing);
    REQUIRE(state.get_print_job_state() == PrintJobState::PRINTING);
    REQUIRE_FALSE(state.can_start_new_print());

    // Track what the string observer sees when state changes to "complete"
    struct ObserverData {
        PrinterState* state;
        bool can_print_when_observed;
        int enum_value_when_observed;
        int fire_count;
    };
    ObserverData data{&state, false, -1, 0};

    auto observer_cb = [](lv_observer_t* observer, lv_subject_t* /*subject*/) {
        auto* d = static_cast<ObserverData*>(lv_observer_get_user_data(observer));
        d->fire_count++;
        d->can_print_when_observed = d->state->can_start_new_print();
        d->enum_value_when_observed = static_cast<int>(d->state->get_print_job_state());
    };

    lv_observer_t* observer =
        lv_subject_add_observer(state.get_print_state_subject(), observer_cb, &data);

    // Initial add notification
    REQUIRE(data.fire_count == 1);

    // Complete the print — the string observer must see COMPLETE enum, not stale PRINTING
    json complete = {{"print_stats", {{"state", "complete"}}}};
    state.update_from_status(complete);

    REQUIRE(data.fire_count >= 2);
    REQUIRE(data.enum_value_when_observed == static_cast<int>(PrintJobState::COMPLETE));
    REQUIRE(data.can_print_when_observed == true);

    // Verify can_start_new_print is true after the full update
    REQUIRE(state.can_start_new_print() == true);

    lv_observer_remove(observer);
}

// ============================================================================
// Reset Cycle Tests
// ============================================================================

TEST_CASE("Print characterization: subjects survive reset_for_testing cycle",
          "[characterization][print][reset]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Set some values
    json status = {{"print_stats", {{"state", "printing"}, {"filename", "test.gcode"}}},
                   {"virtual_sdcard", {{"progress", 0.5}}}};
    state.update_from_status(status);

    // Verify values were set
    REQUIRE(lv_subject_get_int(state.get_print_state_enum_subject()) ==
            static_cast<int>(PrintJobState::PRINTING));
    REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 50);

    // Reset and reinitialize
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // After reset, values should be back to defaults
    REQUIRE(lv_subject_get_int(state.get_print_state_enum_subject()) ==
            static_cast<int>(PrintJobState::STANDBY));
    REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 0);
    REQUIRE(std::string(lv_subject_get_string(state.get_print_filename_subject())) == "");

    // Subjects should still be functional after reset
    json new_status = {{"print_stats", {{"state", "paused"}}}};
    state.update_from_status(new_status);

    REQUIRE(lv_subject_get_int(state.get_print_state_enum_subject()) ==
            static_cast<int>(PrintJobState::PAUSED));
}

TEST_CASE("Print characterization: subject pointers remain valid after reset",
          "[characterization][print][reset]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Capture subject pointers
    lv_subject_t* state_enum_before = state.get_print_state_enum_subject();
    lv_subject_t* progress_before = state.get_print_progress_subject();
    lv_subject_t* outcome_before = state.get_print_outcome_subject();

    // Reset and reinitialize
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Pointers should be the same (singleton subjects are reused)
    lv_subject_t* state_enum_after = state.get_print_state_enum_subject();
    lv_subject_t* progress_after = state.get_print_progress_subject();
    lv_subject_t* outcome_after = state.get_print_outcome_subject();

    REQUIRE(state_enum_before == state_enum_after);
    REQUIRE(progress_before == progress_after);
    REQUIRE(outcome_before == outcome_after);
}

// ============================================================================
// Independence Tests
// ============================================================================

TEST_CASE("Print characterization: print update does not affect non-print subjects",
          "[characterization][print][independence]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Set some non-print values first
    json initial = {{"toolhead", {{"position", {100.0, 200.0, 30.0}}}}};
    state.update_from_status(initial);

    // Positions stored as centimillimeters (×100) for 0.01mm precision
    REQUIRE(lv_subject_get_int(state.get_position_x_subject()) == 10000);

    // Now update print state
    json print_update = {{"print_stats", {{"state", "printing"}, {"filename", "test.gcode"}}},
                         {"virtual_sdcard", {{"progress", 0.5}}}};
    state.update_from_status(print_update);

    // Print values should be updated
    REQUIRE(lv_subject_get_int(state.get_print_state_enum_subject()) ==
            static_cast<int>(PrintJobState::PRINTING));
    REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 50);

    // Position should be unchanged (still centimillimeters)
    REQUIRE(lv_subject_get_int(state.get_position_x_subject()) == 10000);
}

TEST_CASE("Print characterization: non-print update does not affect print subjects",
          "[characterization][print][independence]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Set print values first
    json print_status = {{"print_stats", {{"state", "printing"}, {"filename", "test.gcode"}}},
                         {"virtual_sdcard", {{"progress", 0.75}}}};
    state.update_from_status(print_status);

    REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 75);

    // Now update position (non-print)
    json position_update = {{"toolhead", {{"position", {50.0, 75.0, 10.0}}}}};
    state.update_from_status(position_update);

    // Print values should be unchanged
    REQUIRE(lv_subject_get_int(state.get_print_state_enum_subject()) ==
            static_cast<int>(PrintJobState::PRINTING));
    REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 75);
    REQUIRE(std::string(lv_subject_get_string(state.get_print_filename_subject())) == "test.gcode");
}

// ============================================================================
// get_print_job_state Convenience Method
// ============================================================================

TEST_CASE("Print characterization: get_print_job_state returns correct enum",
          "[characterization][print][api]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("returns STANDBY by default") {
        REQUIRE(state.get_print_job_state() == PrintJobState::STANDBY);
    }

    SECTION("returns PRINTING when printing") {
        json status = {{"print_stats", {{"state", "printing"}}}};
        state.update_from_status(status);

        REQUIRE(state.get_print_job_state() == PrintJobState::PRINTING);
    }

    SECTION("returns PAUSED when paused") {
        json status = {{"print_stats", {{"state", "paused"}}}};
        state.update_from_status(status);

        REQUIRE(state.get_print_job_state() == PrintJobState::PAUSED);
    }

    SECTION("returns COMPLETE when complete") {
        json status = {{"print_stats", {{"state", "complete"}}}};
        state.update_from_status(status);

        REQUIRE(state.get_print_job_state() == PrintJobState::COMPLETE);
    }
}

// ============================================================================
// parse_print_job_state Function Tests
// ============================================================================

TEST_CASE("Print characterization: parse_print_job_state function",
          "[characterization][print][parse]") {
    SECTION("parses 'standby' to STANDBY") {
        REQUIRE(parse_print_job_state("standby") == PrintJobState::STANDBY);
    }

    SECTION("parses 'printing' to PRINTING") {
        REQUIRE(parse_print_job_state("printing") == PrintJobState::PRINTING);
    }

    SECTION("parses 'paused' to PAUSED") {
        REQUIRE(parse_print_job_state("paused") == PrintJobState::PAUSED);
    }

    SECTION("parses 'complete' to COMPLETE") {
        REQUIRE(parse_print_job_state("complete") == PrintJobState::COMPLETE);
    }

    SECTION("parses 'cancelled' to CANCELLED") {
        REQUIRE(parse_print_job_state("cancelled") == PrintJobState::CANCELLED);
    }

    SECTION("parses 'error' to ERROR") {
        REQUIRE(parse_print_job_state("error") == PrintJobState::ERROR);
    }

    SECTION("parses unknown string to STANDBY") {
        REQUIRE(parse_print_job_state("unknown") == PrintJobState::STANDBY);
    }

    SECTION("parses nullptr to STANDBY") {
        REQUIRE(parse_print_job_state(nullptr) == PrintJobState::STANDBY);
    }
}

// ============================================================================
// print_job_state_to_string Function Tests
// ============================================================================

TEST_CASE("Print characterization: print_job_state_to_string function",
          "[characterization][print][string]") {
    REQUIRE(std::string(print_job_state_to_string(PrintJobState::STANDBY)) == "Standby");
    REQUIRE(std::string(print_job_state_to_string(PrintJobState::PRINTING)) == "Printing");
    REQUIRE(std::string(print_job_state_to_string(PrintJobState::PAUSED)) == "Paused");
    REQUIRE(std::string(print_job_state_to_string(PrintJobState::COMPLETE)) == "Complete");
    REQUIRE(std::string(print_job_state_to_string(PrintJobState::CANCELLED)) == "Cancelled");
    REQUIRE(std::string(print_job_state_to_string(PrintJobState::ERROR)) == "Error");
}

// ============================================================================
// Slicer Estimated Print Time Fallback Tests
// ============================================================================

TEST_CASE("Print characterization: slicer estimated_print_time used as fallback",
          "[characterization][print][time][slicer]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("slicer estimate used when print_duration is 0") {
        // Set slicer estimated time (e.g., 83 seconds for a small cube)
        state.set_estimated_print_time(83);
        REQUIRE(state.get_estimated_print_time() == 83);

        // Progress at 5% but no actual print_duration yet
        json progress_status = {{"virtual_sdcard", {{"progress", 0.05}}}};
        state.update_from_status(progress_status);

        json status = {{"print_stats", {{"print_duration", 0.0}, {"total_duration", 30.0}}}};
        state.update_from_status(status);

        // Fallback: 83 * (100-5) / 100 = 83 * 95 / 100 = 78
        REQUIRE(lv_subject_get_int(state.get_print_time_left_subject()) == 78);
    }

    SECTION("progress-based estimate takes over when print_duration > 0") {
        state.set_estimated_print_time(83);

        // Set progress to 25%
        json progress_status = {{"virtual_sdcard", {{"progress", 0.25}}}};
        state.update_from_status(progress_status);

        // Now print_duration has real data - progress-based estimate should be used
        json status = {{"print_stats", {{"print_duration", 20.0}, {"total_duration", 50.0}}}};
        state.update_from_status(status);

        // Progress-based: 20 * (100-25) / 25 = 20 * 3 = 60
        // NOT slicer-based: 83 * 75 / 100 = 62
        REQUIRE(lv_subject_get_int(state.get_print_time_left_subject()) == 60);
    }

    SECTION("estimated_print_time preserved across reset_for_new_print") {
        // estimated_print_time belongs to the FILE, not the print session.
        // It must survive reset so same-file reprints still have the estimate.
        state.set_estimated_print_time(300);
        REQUIRE(state.get_estimated_print_time() == 300);

        state.reset_for_new_print();

        REQUIRE(state.get_estimated_print_time() == 300);
    }

    SECTION("slicer fallback not used when estimated_print_time is 0") {
        // Don't set estimated_print_time (default 0)
        json progress_status = {{"virtual_sdcard", {{"progress", 0.05}}}};
        state.update_from_status(progress_status);

        json status = {{"print_stats", {{"print_duration", 0.0}, {"total_duration", 30.0}}}};
        state.update_from_status(status);

        // No fallback available, should stay at 0
        REQUIRE(lv_subject_get_int(state.get_print_time_left_subject()) == 0);
    }

    SECTION("slicer estimate seeds time_left at progress 0") {
        state.set_estimated_print_time(83);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        // Seeding sets time_left to slicer estimate
        REQUIRE(lv_subject_get_int(state.get_print_time_left_subject()) == 83);

        // Progress at 0%, print_duration at 0 - no condition fires, seeded value persists
        json status = {{"print_stats", {{"print_duration", 0.0}, {"total_duration", 5.0}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.get_print_time_left_subject()) == 83);
    }

    SECTION("low progress blends slicer and progress-based estimates") {
        // At 3% progress with slicer estimate, blend weights: 40% slicer, 60% progress
        state.set_estimated_print_time(2700); // 45 min

        json progress_status = {{"virtual_sdcard", {{"progress", 0.03}}}};
        state.update_from_status(progress_status);

        json status = {{"print_stats", {{"print_duration", 90.0}, {"total_duration", 100.0}}}};
        state.update_from_status(status);

        // Progress-based: 90 * 97 / 3 = 2910
        // Slicer-based: 2700 * 97 / 100 = 2619
        // Blend weight at 3%: (5-3)/5 = 0.4 slicer, 0.6 progress
        // Blended: 0.4 * 2619 + 0.6 * 2910 = 1047.6 + 1746.0 = 2793
        int time_left = lv_subject_get_int(state.get_print_time_left_subject());
        REQUIRE(time_left == 2793);
    }

    SECTION("blend disengages at 5% progress") {
        state.set_estimated_print_time(2700);

        json progress_status = {{"virtual_sdcard", {{"progress", 0.05}}}};
        state.update_from_status(progress_status);

        json status = {{"print_stats", {{"print_duration", 150.0}, {"total_duration", 160.0}}}};
        state.update_from_status(status);

        // At 5%, no blend - pure progress-based: 150 * 95 / 5 = 2850
        REQUIRE(lv_subject_get_int(state.get_print_time_left_subject()) == 2850);
    }

    SECTION("negative estimated_print_time is clamped to 0") {
        state.set_estimated_print_time(-10);
        REQUIRE(state.get_estimated_print_time() == 0);
    }
}

// ============================================================================
// Pre-print Time Remaining Bug Fix Tests
// ============================================================================

TEST_CASE("Print characterization: reset_for_new_print re-seeds time_left from slicer estimate",
          "[characterization][print][time][preprint]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("same-file reprint preserves time_left from slicer estimate") {
        // Simulate first print: slicer says 1469s (24.5 min)
        state.set_estimated_print_time(1469);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        // time_left was seeded with slicer estimate
        REQUIRE(lv_subject_get_int(state.get_print_time_left_subject()) == 1469);

        // Print completes, user reprints same file → reset_for_new_print fires
        state.reset_for_new_print();

        // time_left should be re-seeded from estimated_print_time, NOT cleared to 0
        REQUIRE(lv_subject_get_int(state.get_print_time_left_subject()) == 1469);
        REQUIRE(state.get_estimated_print_time() == 1469);
    }

    SECTION("reset clears progress and duration but keeps estimate") {
        state.set_estimated_print_time(1469);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        // Simulate some print progress
        json progress = {{"virtual_sdcard", {{"progress", 0.50}}}};
        state.update_from_status(progress);
        json stats = {{"print_stats", {{"print_duration", 700.0}, {"total_duration", 750.0}}}};
        state.update_from_status(stats);

        // Verify progress advanced
        REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 50);
        REQUIRE(lv_subject_get_int(state.get_print_duration_subject()) == 700);

        // Reset for new print
        state.reset_for_new_print();

        // Progress/duration cleared
        REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 0);
        REQUIRE(lv_subject_get_int(state.get_print_duration_subject()) == 0);
        REQUIRE(lv_subject_get_int(state.get_print_elapsed_subject()) == 0);

        // But time_left re-seeded and estimate preserved
        REQUIRE(lv_subject_get_int(state.get_print_time_left_subject()) == 1469);
        REQUIRE(state.get_estimated_print_time() == 1469);
    }

    SECTION("reset with no prior estimate sets time_left to 0") {
        // No slicer estimate set (default 0)
        state.reset_for_new_print();

        REQUIRE(lv_subject_get_int(state.get_print_time_left_subject()) == 0);
        REQUIRE(state.get_estimated_print_time() == 0);
    }

    SECTION("different file updates time_left even after reset seeded old value") {
        // First file: 1469s estimate
        state.set_estimated_print_time(1469);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        // Reset (re-seeds with old estimate)
        state.reset_for_new_print();
        REQUIRE(lv_subject_get_int(state.get_print_time_left_subject()) == 1469);

        // New file has different estimate (500s) — metadata callback fires
        state.set_estimated_print_time(500);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        // Progress is still 0, so set_estimated_print_time should update time_left
        REQUIRE(lv_subject_get_int(state.get_print_time_left_subject()) == 500);
        REQUIRE(state.get_estimated_print_time() == 500);
    }

    SECTION("set_estimated_print_time updates time_left at progress 0 even when non-zero") {
        // Seed with initial estimate
        state.set_estimated_print_time(1000);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
        REQUIRE(lv_subject_get_int(state.get_print_time_left_subject()) == 1000);

        // New estimate arrives while still at 0% progress
        state.set_estimated_print_time(2000);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        // Should update to new value (not skip because time_left was already non-zero)
        REQUIRE(lv_subject_get_int(state.get_print_time_left_subject()) == 2000);
    }

    SECTION("set_estimated_print_time does NOT update time_left once progress > 0") {
        state.set_estimated_print_time(1000);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        // Advance progress to 10%
        json progress = {{"virtual_sdcard", {{"progress", 0.10}}}};
        state.update_from_status(progress);
        json stats = {{"print_stats", {{"print_duration", 100.0}, {"total_duration", 110.0}}}};
        state.update_from_status(stats);

        // Progress-based: 100 * 90 / 10 = 900
        int time_left_before = lv_subject_get_int(state.get_print_time_left_subject());
        REQUIRE(time_left_before == 900);

        // Late metadata callback with a different estimate should NOT override
        state.set_estimated_print_time(5000);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        // time_left should still be progress-based, not the new slicer estimate
        REQUIRE(lv_subject_get_int(state.get_print_time_left_subject()) == 900);
    }
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_CASE("Print characterization: edge cases", "[characterization][print][edge]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("empty status does not crash") {
        json empty = json::object();
        state.update_from_status(empty);

        // Values should remain at defaults
        REQUIRE(lv_subject_get_int(state.get_print_state_enum_subject()) ==
                static_cast<int>(PrintJobState::STANDBY));
    }

    SECTION("status with missing print_stats does not crash") {
        json status = {{"toolhead", {{"position", {0.0, 0.0, 0.0}}}}};
        state.update_from_status(status);

        // Print state should remain at default
        REQUIRE(lv_subject_get_int(state.get_print_state_enum_subject()) ==
                static_cast<int>(PrintJobState::STANDBY));
    }

    SECTION("status with missing virtual_sdcard does not crash") {
        json status = {{"print_stats", {{"state", "printing"}}}};
        state.update_from_status(status);

        // Progress should remain at 0
        REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 0);
    }

    SECTION("very long filename is handled") {
        std::string long_name(200, 'a');
        long_name += ".gcode";
        json status = {{"print_stats", {{"filename", long_name}}}};
        state.update_from_status(status);

        // Should be stored (buffer is 256 chars)
        const char* stored = lv_subject_get_string(state.get_print_filename_subject());
        REQUIRE(std::strlen(stored) > 0);
    }
}
