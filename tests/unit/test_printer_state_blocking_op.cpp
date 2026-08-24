// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_printer_state_blocking_op.cpp
 * @brief Tests for the "blocking non-print operation in progress" signal.
 *
 * PrinterState::is_blocking_operation_active() is the predicate that a later
 * send-boundary guard uses to refuse discretionary g-code (fan/temp/LED/moves)
 * while the printer is executing a blocking non-print op (G28, BED_MESH_CALIBRATE,
 * QGL, PROBE_ACCURACY, manual probe, long macro).
 *
 * Signal =
 *     (idle_timeout.state == "Printing" AND print_job_state NOT IN {PRINTING, PAUSED})
 *     OR manual_probe.is_active
 *
 * The idle_timeout.state == "Printing" flag is Klipper's canonical busy indicator,
 * true for the whole duration of ANY blocking command issued from idle. Excluding
 * real file prints (PRINTING/PAUSED) keeps mid-print fan/temp tweaks working.
 *
 * These tests drive the underlying subjects directly (no mock idle_timeout
 * plumbing) and also exercise the JSON parse path in
 * PrinterCalibrationState::update_from_status via PrinterState::update_from_status.
 */

#include "../../include/printer_calibration_state.h"
#include "../../include/printer_state.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/printer_state_test_access.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

class BlockingOpFixture : public LVGLTestFixture {
  public:
    BlockingOpFixture() {
        state.init_subjects(false);
        state.set_klippy_state_sync(KlippyState::READY);
    }

    // Every case here means "a blocking op is under way", i.e. one that has
    // already outlasted the IdleTimeoutBusy settle window. Back-date the
    // debounce so these read as sustained rather than just-started; the settle
    // behaviour itself is covered in test_idle_timeout_busy.cpp.
    void set_idle_timeout_printing(int v) {
        helix::PrinterStateTestAccess::set_sustained_idle_timeout_printing(state, v != 0);
    }

    void set_manual_probe(int v) {
        lv_subject_set_int(state.get_manual_probe_active_subject(), v);
    }

    void set_print_state(PrintJobState s) {
        lv_subject_set_int(state.get_print_state_enum_subject(), static_cast<int>(s));
    }

    PrinterState state;
};

} // namespace

// ============================================================================
// Case 1: predicate truth table
// ============================================================================

TEST_CASE_METHOD(BlockingOpFixture,
                 "is_blocking_operation_active reflects idle_timeout + print state",
                 "[printer_state][blocking_op]") {
    SECTION("idle everything -> not blocking") {
        set_idle_timeout_printing(0);
        set_manual_probe(0);
        set_print_state(PrintJobState::STANDBY);
        CHECK_FALSE(state.is_blocking_operation_active());
    }

    SECTION("idle_timeout Printing while STANDBY -> blocking (homing/leveling)") {
        set_idle_timeout_printing(1);
        set_manual_probe(0);
        set_print_state(PrintJobState::STANDBY);
        CHECK(state.is_blocking_operation_active());
    }

    SECTION("idle_timeout Printing during a real file print -> NOT blocking") {
        set_idle_timeout_printing(1);
        set_manual_probe(0);
        set_print_state(PrintJobState::PRINTING);
        CHECK_FALSE(state.is_blocking_operation_active());
    }

    SECTION("idle_timeout Printing while PAUSED -> NOT blocking") {
        set_idle_timeout_printing(1);
        set_manual_probe(0);
        set_print_state(PrintJobState::PAUSED);
        CHECK_FALSE(state.is_blocking_operation_active());
    }

    SECTION("manual probe active -> blocking regardless of print state") {
        set_manual_probe(1);

        set_idle_timeout_printing(0);
        set_print_state(PrintJobState::PRINTING);
        CHECK(state.is_blocking_operation_active());

        set_print_state(PrintJobState::PAUSED);
        CHECK(state.is_blocking_operation_active());

        set_print_state(PrintJobState::STANDBY);
        CHECK(state.is_blocking_operation_active());
    }
}

// ============================================================================
// Case 1b: is_external_blocking_operation_active attributes self-inflicted busy
// ============================================================================

TEST_CASE_METHOD(BlockingOpFixture, "is_external_blocking_operation_active attributes self-busy",
                 "[printer_state][busy_guard]") {
    // Arrange like the "idle_timeout Printing while STANDBY -> blocking" section:
    // idle_timeout_printing = 1, print job state STANDBY, no manual probe.
    set_idle_timeout_printing(1);
    set_manual_probe(0);
    set_print_state(PrintJobState::STANDBY);

    SECTION("external busy: no app motion -> blocked") {
        CHECK(state.is_blocking_operation_active());
        CHECK(state.is_external_blocking_operation_active());
    }

    SECTION("self busy: app motion in flight -> not blocked") {
        state.app_motion_activity().note_sent();
        CHECK(state.is_blocking_operation_active()); // raw predicate unchanged
        CHECK_FALSE(state.is_external_blocking_operation_active());
        state.app_motion_activity().note_done();
    }

    SECTION("manual probe blocks even during app motion") {
        set_manual_probe(1);
        state.app_motion_activity().note_sent();
        CHECK(state.is_external_blocking_operation_active());
        state.app_motion_activity().note_done();
    }
}

// ============================================================================
// Case 2: idle_timeout.state JSON parse -> idle_timeout_printing_ subject
// ============================================================================

TEST_CASE_METHOD(BlockingOpFixture, "update_from_status parses idle_timeout.state into subject",
                 "[printer_state][blocking_op]") {
    lv_subject_t* subj = state.get_idle_timeout_printing_subject();

    SECTION("state == Printing -> 1") {
        nlohmann::json status = {{"idle_timeout", {{"state", "Printing"}}}};
        state.update_from_status(status);
        CHECK(lv_subject_get_int(subj) == 1);
    }

    SECTION("state == Ready -> 0") {
        // First drive it high, then confirm Ready lowers it (exercise the transition).
        lv_subject_set_int(subj, 1);
        nlohmann::json status = {{"idle_timeout", {{"state", "Ready"}}}};
        state.update_from_status(status);
        CHECK(lv_subject_get_int(subj) == 0);
    }

    SECTION("state == Idle -> 0") {
        lv_subject_set_int(subj, 1);
        nlohmann::json status = {{"idle_timeout", {{"state", "Idle"}}}};
        state.update_from_status(status);
        CHECK(lv_subject_get_int(subj) == 0);
    }
}

// ============================================================================
// Case 3: once-per-episode busy-queue toast latch (#1108)
// ============================================================================
//
// When benign discretionary gcode queues behind a blocking op, the guard should
// tell the user ONCE per episode, not once per command. claim_busy_queue_toast()
// returns true for the first claim after the op starts, false thereafter, and
// re-arms on the op's falling edge (idle_timeout Ready, or manual_probe inactive).

TEST_CASE_METHOD(BlockingOpFixture, "claim_busy_queue_toast fires once per blocking episode",
                 "[printer_state][busy_guard]") {
    // The re-arm consults is_blocking_operation_active(), which excludes real file
    // prints — keep the fixture in a non-print state so the blocking signals apply.
    set_print_state(PrintJobState::STANDBY);

    auto idle_timeout = [&](const char* s) {
        state.update_from_status(nlohmann::json{{"idle_timeout", {{"state", s}}}});
    };
    auto manual_probe = [&](bool active) {
        state.update_from_status(nlohmann::json{{"manual_probe", {{"is_active", active}}}});
    };

    SECTION("idle_timeout episode: claimed once, then re-armed after it ends") {
        idle_timeout("Printing"); // episode 1 begins
        CHECK(state.claim_busy_queue_toast());
        CHECK_FALSE(state.claim_busy_queue_toast());
        CHECK_FALSE(state.claim_busy_queue_toast());

        idle_timeout("Ready");    // op flushes -> re-arm
        idle_timeout("Printing"); // episode 2 begins
        CHECK(state.claim_busy_queue_toast());
    }

    SECTION("manual-probe episode re-arms once it clears") {
        manual_probe(true); // probe episode begins (idle_timeout stays Ready/0)
        CHECK(state.claim_busy_queue_toast());
        CHECK_FALSE(state.claim_busy_queue_toast());

        manual_probe(false); // probe done -> composite clears -> re-arm
        manual_probe(true);  // new probe episode
        CHECK(state.claim_busy_queue_toast());
    }

    SECTION("idle_timeout bounce mid manual-probe does NOT re-toast (composite episode)") {
        // A PROBE_CALIBRATE / TESTZ session: manual_probe holds the block, but
        // idle_timeout bounces Printing->Ready between TESTZ moves. That bounce must
        // NOT re-arm the toast — it is still one episode (#1108 review, Finding 1).
        manual_probe(true);
        idle_timeout("Printing");
        CHECK(state.claim_busy_queue_toast()); // first tap -> toast
        CHECK_FALSE(state.claim_busy_queue_toast());

        idle_timeout("Ready");                       // idle bounce, probe still active
        CHECK_FALSE(state.claim_busy_queue_toast()); // STILL suppressed
        idle_timeout("Printing");                    // next TESTZ move
        CHECK_FALSE(state.claim_busy_queue_toast()); // STILL the same episode

        // Episode ends only when BOTH signals clear.
        manual_probe(false);
        idle_timeout("Ready");
        idle_timeout("Printing"); // a fresh homing/leveling episode
        CHECK(state.claim_busy_queue_toast());
    }
}

// ============================================================================
// Klippy-volatile state (#1129)
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "reset_klippy_volatile clears activity state but leaves config alone",
                 "[printer_state][volatile][1129]") {
    helix::PrinterCalibrationState cs;
    cs.init_subjects(false);

    // Activity state, all driven away from their defaults.
    lv_subject_set_int(cs.get_idle_timeout_printing_subject(), 1);
    lv_subject_set_int(cs.get_manual_probe_active_subject(), 1);
    lv_subject_set_int(cs.get_manual_probe_z_position_subject(), 125);
    lv_subject_set_int(cs.get_motors_enabled_subject(), 0);
    // Config-derived: NOT volatile, must survive.
    lv_subject_set_int(cs.get_retract_length_subject(), 80);

    cs.reset_klippy_volatile();

    CHECK(lv_subject_get_int(cs.get_idle_timeout_printing_subject()) == 0);
    CHECK(lv_subject_get_int(cs.get_manual_probe_active_subject()) == 0);
    CHECK(lv_subject_get_int(cs.get_manual_probe_z_position_subject()) == 0);
    // motors_enabled is NOT Klippy-volatile: right after a Klipper shutdown the
    // steppers are affirmatively de-energized, so a value set before the reset
    // must survive it unchanged rather than bouncing back to the enabled default.
    CHECK(lv_subject_get_int(cs.get_motors_enabled_subject()) == 0);
    CHECK(lv_subject_get_int(cs.get_retract_length_subject()) == 80);
}

TEST_CASE_METHOD(LVGLTestFixture, "re-running init_subjects does not duplicate volatile entries",
                 "[printer_state][volatile][1129]") {
    helix::PrinterCalibrationState cs;
    cs.init_subjects(false);
    cs.init_subjects(false);

    lv_subject_set_int(cs.get_idle_timeout_printing_subject(), 1);
    cs.reset_klippy_volatile();
    CHECK(lv_subject_get_int(cs.get_idle_timeout_printing_subject()) == 0);
}

TEST_CASE_METHOD(BlockingOpFixture,
                 "a Klippy restart clears a stale idle_timeout busy flag (#1129)",
                 "[printer_state][blocking_op][1129]") {
    // Klipper is mid-G28: idle_timeout says "Printing" with no file print.
    set_idle_timeout_printing(1);
    REQUIRE(state.is_blocking_operation_active());

    // Klipper dies. Everything it was doing died with it.
    state.set_klippy_state_sync(KlippyState::SHUTDOWN);

    CHECK_FALSE(state.is_blocking_operation_active());
}

TEST_CASE_METHOD(BlockingOpFixture,
                 "returning to READY clears a busy flag that went stale while down (#1129)",
                 "[printer_state][blocking_op][1129]") {
    // This is the reporter's exact shape: the stale value survived PAST the return
    // to READY, which is why a live `klippy != READY` predicate would not have helped.
    state.set_klippy_state_sync(KlippyState::SHUTDOWN);
    set_idle_timeout_printing(1);

    state.set_klippy_state_sync(KlippyState::READY);

    CHECK_FALSE(state.is_blocking_operation_active());
}

TEST_CASE_METHOD(BlockingOpFixture, "a stale manual_probe flag is cleared by a Klippy restart",
                 "[printer_state][blocking_op][1129]") {
    // A klippy crash mid-PROBE_CALIBRATE wedges the same gate by a different input.
    set_manual_probe(1);
    REQUIRE(state.is_blocking_operation_active());

    state.set_klippy_state_sync(KlippyState::SHUTDOWN);

    CHECK_FALSE(state.is_blocking_operation_active());
}

TEST_CASE_METHOD(BlockingOpFixture,
                 "a webhooks-driven Klippy transition also clears stale busy state",
                 "[printer_state][blocking_op][1129]") {
    // The JSON path must reach the same hook as set_klippy_state_sync. Before the
    // two paths are unified this fails: printer_state.cpp:459 bypasses the hook.
    set_idle_timeout_printing(1);
    REQUIRE(state.is_blocking_operation_active());

    nlohmann::json status = {{"webhooks", {{"state", "shutdown"}}}};
    state.update_from_status(status);

    CHECK_FALSE(state.is_blocking_operation_active());
}

TEST_CASE_METHOD(BlockingOpFixture,
                 "fresh idle_timeout in the same payload wins over the transition reset",
                 "[printer_state][blocking_op][1129]") {
    // Ordering guarantee: the webhooks block resets, THEN the calibration parse
    // applies fresh data. A payload that says both "ready" and "Printing" must end
    // up busy, not reset to idle.
    state.set_klippy_state_sync(KlippyState::SHUTDOWN);
    set_idle_timeout_printing(0);

    nlohmann::json status = {{"webhooks", {{"state", "ready"}}},
                             {"idle_timeout", {{"state", "Printing"}}}};
    state.update_from_status(status);

    CHECK(lv_subject_get_int(state.get_idle_timeout_printing_subject()) == 1);
}

TEST_CASE_METHOD(BlockingOpFixture, "a repeated Klippy state is not treated as a transition",
                 "[printer_state][blocking_op][1129]") {
    // Guards against resetting on every status payload, which would stomp a
    // legitimately-busy printer mid-G28.
    set_idle_timeout_printing(1);
    state.set_klippy_state_sync(KlippyState::READY); // already READY — no edge

    CHECK(state.is_blocking_operation_active());
}
