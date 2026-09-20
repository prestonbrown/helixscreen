// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_slicer_progress.cpp
 * @brief TDD red-phase tests for slicer-preferred progress (Issue #122)
 *
 * HelixScreen currently uses virtual_sdcard.progress (file-position-based)
 * which produces inaccurate time estimates. These tests verify that
 * display_status.progress (M73 slicer time-weighted) is preferred when
 * available, with fallback to file-based progress when no slicer data exists.
 *
 * The slicer progress feature:
 * - display_status.progress overrides virtual_sdcard.progress when non-zero
 * - Slicer activates on first non-zero display_status.progress value
 * - Once active, virtual_sdcard-only updates do NOT override slicer progress
 * - Slicer active flag resets when a new print starts
 * - Terminal state guard still applies to slicer progress
 * - Time estimation uses slicer progress when active
 */

#include "../test_helpers/printer_state_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;
using json = nlohmann::json;

// ============================================================================
// Slicer Preference When Active
// ============================================================================

TEST_CASE("Slicer progress: display_status overrides virtual_sdcard when non-zero",
          "[print][progress][slicer]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Start a print
    json printing = {{"print_stats", {{"state", "printing"}}}};
    state.update_from_status(printing);

    // Send both virtual_sdcard and display_status in same update
    // display_status.progress (slicer) should be preferred over virtual_sdcard
    json status = {{"virtual_sdcard", {{"progress", 0.5}}},
                   {"display_status", {{"progress", 0.3}}}};
    state.update_from_status(status);

    // Slicer says 30%, file says 50% -- slicer should win
    REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 30);
}

TEST_CASE("Slicer progress: activates on first non-zero display_status value",
          "[print][progress][slicer]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Start a print
    json printing = {{"print_stats", {{"state", "printing"}}}};
    state.update_from_status(printing);

    // First update: display_status is 0 -- use virtual_sdcard (file)
    json status1 = {{"virtual_sdcard", {{"progress", 0.1}}},
                    {"display_status", {{"progress", 0.0}}}};
    state.update_from_status(status1);

    // With display_status at 0, file progress should be used
    REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 10);

    // Second update: display_status becomes non-zero -- switch to slicer
    json status2 = {{"virtual_sdcard", {{"progress", 0.15}}},
                    {"display_status", {{"progress", 0.08}}}};
    state.update_from_status(status2);

    // Slicer activated: 8% from slicer, not 15% from file
    REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 8);
}

TEST_CASE("Slicer progress: virtual_sdcard-only updates do not override slicer once active",
          "[print][progress][slicer]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Start a print
    json printing = {{"print_stats", {{"state", "printing"}}}};
    state.update_from_status(printing);

    // Activate slicer with non-zero display_status
    json activate = {{"virtual_sdcard", {{"progress", 0.2}}},
                     {"display_status", {{"progress", 0.15}}}};
    state.update_from_status(activate);
    REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 15);

    // Now send only virtual_sdcard update (no display_status)
    // Slicer is authoritative -- progress should NOT change to file value
    json file_only = {{"virtual_sdcard", {{"progress", 0.6}}}};
    state.update_from_status(file_only);

    // Should still show slicer value (15%), not file value (60%)
    REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 15);
}

TEST_CASE("Slicer progress: slicer-only update advances progress", "[print][progress][slicer]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Start a print
    json printing = {{"print_stats", {{"state", "printing"}}}};
    state.update_from_status(printing);

    // Activate slicer
    json activate = {{"virtual_sdcard", {{"progress", 0.2}}},
                     {"display_status", {{"progress", 0.15}}}};
    state.update_from_status(activate);
    REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 15);

    // New display_status update advances progress
    json slicer_update = {{"display_status", {{"progress", 0.25}}}};
    state.update_from_status(slicer_update);

    REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 25);
}

// ============================================================================
// Fallback -- No Slicer Data
// ============================================================================

TEST_CASE("Slicer progress: virtual_sdcard used when display_status never appears",
          "[print][progress][slicer]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Start a print
    json printing = {{"print_stats", {{"state", "printing"}}}};
    state.update_from_status(printing);

    // Only virtual_sdcard, no display_status at all
    json status = {{"virtual_sdcard", {{"progress", 0.45}}}};
    state.update_from_status(status);

    // File progress should be used as fallback
    REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 45);
}

TEST_CASE("Slicer progress: virtual_sdcard used when display_status.progress stays at 0",
          "[print][progress][slicer]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Start a print
    json printing = {{"print_stats", {{"state", "printing"}}}};
    state.update_from_status(printing);

    // display_status present but progress stays 0 (no M73 in gcode)
    json status = {{"virtual_sdcard", {{"progress", 0.35}}},
                   {"display_status", {{"progress", 0.0}}}};
    state.update_from_status(status);

    // File progress should be used since slicer never activated
    REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 35);
}

// ============================================================================
// Reset on New Print
// ============================================================================

TEST_CASE("Slicer progress: slicer active flag resets on new print", "[print][progress][slicer]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // First print: activate slicer
    json printing = {{"print_stats", {{"state", "printing"}}}};
    state.update_from_status(printing);

    json activate = {{"virtual_sdcard", {{"progress", 0.3}}},
                     {"display_status", {{"progress", 0.2}}}};
    state.update_from_status(activate);
    REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 20);

    // Print completes
    json complete = {{"print_stats", {{"state", "complete"}}}};
    state.update_from_status(complete);

    // Transition to standby
    json standby = {{"print_stats", {{"state", "standby"}}}};
    state.update_from_status(standby);

    // Start new print (no M73 this time)
    json new_print = {{"print_stats", {{"state", "printing"}}}};
    state.update_from_status(new_print);

    // Only file progress, no display_status -- slicer should NOT be active
    json file_only = {{"virtual_sdcard", {{"progress", 0.4}}}};
    state.update_from_status(file_only);

    // Slicer flag was reset, so file progress should be used
    REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 40);
}

// ============================================================================
// Terminal State Guard
// ============================================================================

TEST_CASE("Slicer progress: cannot go backward in COMPLETE state", "[print][progress][slicer]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Print with slicer active at high progress
    json printing = {{"print_stats", {{"state", "printing"}}},
                     {"virtual_sdcard", {{"progress", 0.95}}},
                     {"display_status", {{"progress", 0.98}}}};
    state.update_from_status(printing);
    REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 98);

    // Complete the print
    json complete = {{"print_stats", {{"state", "complete"}}}};
    state.update_from_status(complete);

    // Try to reset via display_status (shouldn't go backward)
    json reset = {{"display_status", {{"progress", 0.0}}}};
    state.update_from_status(reset);

    // Progress should stay at 98 (terminal guard)
    REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 98);
}

// ============================================================================
// Time Estimation Uses Slicer Progress
// ============================================================================

TEST_CASE("Slicer progress: time estimation uses slicer progress when active",
          "[print][progress][slicer]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Start a print
    json printing = {{"print_stats", {{"state", "printing"}}}};
    state.update_from_status(printing);

    // Activate slicer at 50% with 600s print_duration
    json status = {{"virtual_sdcard", {{"progress", 0.8}}},
                   {"display_status", {{"progress", 0.5}}}};
    state.update_from_status(status);

    json time = {{"print_stats", {{"print_duration", 600.0}, {"total_duration", 620.0}}}};
    state.update_from_status(time);

    // remaining = print_duration * (100 - progress) / progress
    // With slicer at 50%: 600 * (100 - 50) / 50 = 600
    // NOT with file at 80%: 600 * (100 - 80) / 80 = 150
    REQUIRE(lv_subject_get_int(state.get_print_time_left_subject()) == 600);
}

TEST_CASE("Slicer progress: time estimation uses file progress when slicer inactive",
          "[print][progress][slicer]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Start a print -- no slicer
    json printing = {{"print_stats", {{"state", "printing"}}}};
    state.update_from_status(printing);

    json status = {{"virtual_sdcard", {{"progress", 0.8}}}};
    state.update_from_status(status);

    json time = {{"print_stats", {{"print_duration", 600.0}, {"total_duration", 620.0}}}};
    state.update_from_status(time);

    // remaining = 600 * (100 - 80) / 80 = 150
    REQUIRE(lv_subject_get_int(state.get_print_time_left_subject()) == 150);
}

// ============================================================================
// A Paused Print Does Not Advance
// ============================================================================

/*
 * Klipper expires the M73 value behind display_status.progress and substitutes
 * virtual_sdcard's byte position into the same field. A pause is exactly when
 * M73 stops being refreshed, so the field a paused HelixScreen reads as "the
 * slicer estimate" is really file position -- and on a multi-material print the
 * two disagree by a wide margin, because the wipe tower is byte-dense and quick
 * to print. Progress cannot advance while paused, so nothing arriving in that
 * window is a new position, whichever source it came from.
 */

TEST_CASE("Paused progress: a differing payload cannot move the bar",
          "[print][progress][slicer][paused]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    json printing = {{"print_stats", {{"state", "printing"}}}};
    state.update_from_status(printing);
    state.update_from_status(json{{"display_status", {{"progress", 0.11}}}});
    REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 11);

    state.update_from_status(json{{"print_stats", {{"state", "paused"}}}});

    // The same field now carries byte position instead of the slicer estimate.
    state.update_from_status(json{{"display_status", {{"progress", 0.25}}}});
    CHECK(lv_subject_get_int(state.get_print_progress_subject()) == 11);

    // The bar the user watches binds the display subject, so it has to hold too.
    CHECK(lv_subject_get_int(state.get_print_progress_display_subject()) == 11);
}

TEST_CASE("Paused progress: file position cannot move the bar either",
          "[print][progress][slicer][paused]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    json printing = {{"print_stats", {{"state", "printing"}}}};
    state.update_from_status(printing);
    state.update_from_status(json{{"virtual_sdcard", {{"progress", 0.11}}}});
    REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 11);

    state.update_from_status(json{{"print_stats", {{"state", "paused"}}}});
    state.update_from_status(json{{"virtual_sdcard", {{"progress", 0.25}}}});

    // Both writers are judged by the same rules, so neither gets a free pass.
    CHECK(lv_subject_get_int(state.get_print_progress_subject()) == 11);
}

TEST_CASE("Paused progress: tracking resumes once the print does",
          "[print][progress][slicer][paused]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    json printing = {{"print_stats", {{"state", "printing"}}}};
    state.update_from_status(printing);
    state.update_from_status(json{{"display_status", {{"progress", 0.11}}}});

    state.update_from_status(json{{"print_stats", {{"state", "paused"}}}});
    state.update_from_status(json{{"display_status", {{"progress", 0.25}}}});
    REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 11);

    // Holding while paused must not latch. A pause that permanently froze the
    // bar would trade a bouncing number for a dead one.
    state.update_from_status(printing);
    state.update_from_status(json{{"display_status", {{"progress", 0.12}}}});
    CHECK(lv_subject_get_int(state.get_print_progress_subject()) == 12);
}

TEST_CASE("Paused progress: a message-only payload after resume cannot publish the paused value",
          "[print][progress][slicer][paused]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    json printing = {{"print_stats", {{"state", "printing"}}}};
    state.update_from_status(printing);
    state.update_from_status(json{{"display_status", {{"progress", 0.11}}}});
    REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 11);

    // Paused, and the field starts carrying byte position instead of M73.
    state.update_from_status(json{{"print_stats", {{"state", "paused"}}}});
    state.update_from_status(json{{"display_status", {{"progress", 0.25}}}});
    REQUIRE(lv_subject_get_int(state.get_print_progress_subject()) == 11);

    state.update_from_status(printing);

    // An M117 update carries display_status with no progress field, so it
    // refreshes nothing yet still reaches the publish. Declining to record the
    // paused reading in the first place is what stops it arriving here.
    state.update_from_status(json{{"display_status", {{"message", "Resuming"}}}});
    CHECK(lv_subject_get_int(state.get_print_progress_subject()) == 11);
}
