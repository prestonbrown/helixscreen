// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// SAVE_CONFIG never acks. Klipper's cmd_SAVE_CONFIG writes printer.cfg and then
// calls request_restart('restart') as its last act, so Moonraker fails the
// pending printer.gcode.script with 503 "Klippy Disconnected" every single time.
// Panels that believed that error reported "Failed to save configuration" on
// every SUCCESSFUL save, on every printer (prestonbrown/helixscreen#1359).
//
// What is pinned here: the latch that tells a restart apart from a hang, the
// bounded timeout extension built on it, and the rule that decides which rpc
// errors are the expected drop and which are real failures. The latch and the
// timeout helper moved here from z_offset_utils when the third and fourth panel
// needed them; their K2/CFS regression comments moved with them.

#include "save_config_restart.h"

#include "../catch_amalgamated.hpp"

using helix::ui::SaveRestartLatch;
using helix::ui::should_extend_save_timeout;

// ============================================================================
// SaveRestartLatch tests
// ============================================================================
//
// Observed K2 timeline this models:
//   t=0    SAVE_CONFIG sent, panel enters SAVING, Klipper begins restarting
//   t~15s  klippy back READY  -> save succeeded, panel must settle here
//   t~53s  chained CXSAVE_CONFIG restart (already settled; not our problem)
//
// The latch exists because sampling is_expected_restart() at guard-fire time
// (t=30s) always reads false — the 15s suppression window has closed — so the
// extension gate never opened and the panel failed a save that had succeeded.

TEST_CASE("SaveRestartLatch: starts clean", "[save_config][save_latch][1359]") {
    SaveRestartLatch latch;
    REQUIRE_FALSE(latch.restart_latched());
    REQUIRE_FALSE(latch.restart_completed());
}

TEST_CASE("SaveRestartLatch: latches when klippy leaves READY", "[save_config][save_latch][1359]") {
    SaveRestartLatch latch;
    latch.on_klippy_ready(false); // SAVE_CONFIG restart begins
    REQUIRE(latch.restart_latched());
    // Restart began but has not finished — not yet evidence of success.
    REQUIRE_FALSE(latch.restart_completed());
}

TEST_CASE("SaveRestartLatch: latch survives until reset (the whole point)",
          "[save_config][save_latch][1359]") {
    SaveRestartLatch latch;
    latch.on_klippy_ready(false);
    latch.on_klippy_ready(true);

    // At guard-fire time, long after the restart settled and the suppression
    // window closed, the latch must STILL report the restart. An instantaneous
    // is_expected_restart() sample reads false here — that was the inert bug.
    REQUIRE(latch.restart_latched());
    REQUIRE(should_extend_save_timeout(latch.restart_latched(), 0, 4));
}

TEST_CASE("SaveRestartLatch: READY after a restart signals save success",
          "[save_config][save_latch][1359]") {
    SaveRestartLatch latch;
    latch.on_klippy_ready(false); // t=0 restart begins
    REQUIRE_FALSE(latch.restart_completed());

    latch.on_klippy_ready(true); // t~15s Klipper back
    // This is what lets the panel settle at ~15s instead of burning 4x30s of
    // extensions and then failing a save that worked.
    REQUIRE(latch.restart_completed());
}

TEST_CASE("SaveRestartLatch: READY without a preceding restart is not success",
          "[save_config][save_latch][1359]") {
    SaveRestartLatch latch;
    // Klipper was READY all along — the save never restarted anything, so a
    // READY sample must not be mistaken for a completed save.
    latch.on_klippy_ready(true);
    latch.on_klippy_ready(true);
    REQUIRE_FALSE(latch.restart_latched());
    REQUIRE_FALSE(latch.restart_completed());
}

TEST_CASE("SaveRestartLatch: note_restart_expected folds in the suppression window",
          "[save_config][save_latch][1359]") {
    SaveRestartLatch latch;
    latch.note_restart_expected(false);
    REQUIRE_FALSE(latch.restart_latched());

    latch.note_restart_expected(true);
    REQUIRE(latch.restart_latched());

    // Monotonic within a save — a later false must not clear it.
    latch.note_restart_expected(false);
    REQUIRE(latch.restart_latched());
}

TEST_CASE("SaveRestartLatch: reset clears both flags for a second save",
          "[save_config][save_latch][1359]") {
    SaveRestartLatch latch;
    latch.on_klippy_ready(false);
    latch.on_klippy_ready(true);
    REQUIRE(latch.restart_latched());
    REQUIRE(latch.restart_completed());

    latch.reset();
    REQUIRE_FALSE(latch.restart_latched());
    REQUIRE_FALSE(latch.restart_completed());
}

TEST_CASE("SaveRestartLatch: a second save does not inherit the first save's latch",
          "[save_config][save_latch][1359]") {
    // Real failure mode: a sticky latch would make save #2 immediately look like
    // it had restarted Klipper, so a genuinely hung second save would extend its
    // timeout instead of failing, and a stray READY would report false success.
    SaveRestartLatch latch;

    // Save #1: restarts and completes.
    latch.on_klippy_ready(false);
    latch.on_klippy_ready(true);
    REQUIRE(latch.restart_completed());

    // Save #2 begins (panel re-enters SAVING -> reset).
    latch.reset();

    // Klipper never dips; save #2 hangs. Must NOT extend, must NOT look complete.
    latch.on_klippy_ready(true);
    REQUIRE_FALSE(latch.restart_completed());
    REQUIRE_FALSE(latch.restart_latched());
    REQUIRE_FALSE(should_extend_save_timeout(latch.restart_latched(), 0, 4));
}

TEST_CASE("SaveRestartLatch: hung save with no restart still fails terminally",
          "[save_config][save_latch][1359]") {
    SaveRestartLatch latch;
    // Nothing observed at all — the genuinely-hung case.
    REQUIRE_FALSE(should_extend_save_timeout(latch.restart_latched(), 0, 4));
}

TEST_CASE("SaveRestartLatch: latched save still exhausts its extension budget",
          "[save_config][save_latch][1359]") {
    SaveRestartLatch latch;
    latch.on_klippy_ready(false); // restart began and never came back

    REQUIRE(should_extend_save_timeout(latch.restart_latched(), 3, 4));
    // Budget exhausted -> terminal failure even though the latch is set.
    REQUIRE_FALSE(should_extend_save_timeout(latch.restart_latched(), 4, 4));
}
// ============================================================================
// should_extend_save_timeout tests
// ============================================================================
//
// Regression cover for the Creality K2 + CFS save path: SAVE_CONFIG restarts
// Klipper, and Creality's motor_control_wrapper.py chains a second config write
// (CFS Tn_data via CXSAVE_CONFIG) ~50s later. A fixed 30s SAVING guard fired
// mid-restart and flipped the panel to ERROR with "Z-offset calibration timed
// out" even though the save had succeeded.

TEST_CASE("should_extend_save_timeout: extends while a restart is expected",
          "[save_config][save_timeout][1359]") {
    // This is the K2/CFS case — the guard must NOT fail the operation.
    REQUIRE(should_extend_save_timeout(/*expected_restart=*/true, /*extensions_used=*/0,
                                       /*max_extensions=*/4));
    REQUIRE(should_extend_save_timeout(true, 3, 4));
}

TEST_CASE("should_extend_save_timeout: does not extend when no restart is expected",
          "[save_config][save_timeout][1359]") {
    // Genuinely hung save — a real timeout must still be reported. If this
    // returned true the guard would be effectively deleted.
    REQUIRE_FALSE(should_extend_save_timeout(/*expected_restart=*/false, 0, 4));
    REQUIRE_FALSE(should_extend_save_timeout(false, 3, 4));
}

TEST_CASE("should_extend_save_timeout: extension budget is bounded",
          "[save_config][save_timeout][1359]") {
    // Even with a restart perpetually 'expected', extensions must run out so the
    // panel cannot spin forever.
    REQUIRE_FALSE(should_extend_save_timeout(true, 4, 4));
    REQUIRE_FALSE(should_extend_save_timeout(true, 5, 4));
    REQUIRE_FALSE(should_extend_save_timeout(true, 99, 4));
}

TEST_CASE("should_extend_save_timeout: zero budget never extends",
          "[save_config][save_timeout][1359]") {
    REQUIRE_FALSE(should_extend_save_timeout(true, 0, 0));
}
