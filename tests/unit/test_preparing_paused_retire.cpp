// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_preparing_paused_retire.cpp
 * @brief A paused print settles the preparing claim (#1365).
 *
 * reconcile_preparing() gated on PrintJobState::PRINTING alone, so a printer
 * that reports `paused` while a job is preparing never settled the claim. The
 * job stayed armed until the 30 minute watchdog fired TimedOut, and that path
 * cools both heaters.
 *
 * Reported against a Voron whose PRINT_START issues M25 and finishes the start
 * sequence from [delayed_gcode], so print_stats reads `paused` for the whole of
 * a 65 minute ABS soak. The bed was switched off mid-soak. Klipper reaches
 * `paused` only from a job it has accepted, and three other layers already read
 * it that way - derive_print_state() makes PAUSED the sole exception to "a live
 * pre-print phase outranks the job state", and print_control_view routes Stop on
 * PRINTING || PAUSED. Only the claim layer disagreed.
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "print_completion.h"
#include "print_job_ref.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

struct PreparingPausedFixture : public LVGLTestFixture {
    PreparingPausedFixture() {
        state_.init_subjects(false);
    }

    ~PreparingPausedFixture() override {
        state_.deinit_subjects();
    }

    PrinterState& state() {
        return state_;
    }

    void drain() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    /// Drive print_stats through the real parse. reconcile_preparing() is only
    /// reachable from update_from_status(), so calling it directly would assert
    /// against a path production never takes.
    void report(const std::string& job_state, const std::string& filename) {
        nlohmann::json status = {{"print_stats", {{"state", job_state}, {"filename", filename}}}};
        state_.update_from_status(status);
        drain();
    }

    PrinterState state_;
};

} // namespace

TEST_CASE_METHOD(PreparingPausedFixture, "A paused print carrying our filename confirms the claim",
                 "[print][preparing][1365]") {
    state().begin_preparing(PrintJobRef{"KNX-Deckel_ABS.gcode", "", ""});
    REQUIRE(state().has_preparing_job());

    // The reporter's soak: M25 in PRINT_START, so this is the only job state
    // Moonraker ever publishes between start and the closing M24.
    report("paused", "KNX-Deckel_ABS.gcode");

    REQUIRE_FALSE(state().has_preparing_job());
    REQUIRE(state().last_preparing_exit() == PreparingExit::Confirmed);
}

TEST_CASE_METHOD(PreparingPausedFixture, "Confirming from paused leaves the heaters alone",
                 "[print][preparing][1365]") {
    state().begin_preparing(PrintJobRef{"soak.gcode", "", ""});
    report("paused", "soak.gcode");

    // Pin the retirement first. last_preparing_exit_ is initialised to Confirmed,
    // so asserting cool_down alone passes on a build where reconcile never ran.
    REQUIRE_FALSE(state().has_preparing_job());
    REQUIRE(state().last_preparing_exit() == PreparingExit::Confirmed);

    // The bug's actual damage. TimedOut and Cancelled both cool down; Confirmed
    // must not, or the soak loses its bed the moment the claim retires.
    REQUIRE_FALSE(decide_preparing_exit_action(state().last_preparing_exit()).cool_down);
}

TEST_CASE_METHOD(PreparingPausedFixture, "A settled claim disarms the preparing watchdog",
                 "[print][preparing][1365]") {
    auto& pps = PrinterStateTestAccess::get_print_state(state());

    state().begin_preparing(PrintJobRef{"soak.gcode", "", ""});
    REQUIRE(PrinterPrintStateTestAccess::has_preparing_watchdog(pps));

    report("paused", "soak.gcode");

    // The 30 minute timeout is now unreachable for this print: nothing is armed
    // to fire. This is the assertion that pins the reported failure shut.
    REQUIRE_FALSE(PrinterPrintStateTestAccess::has_preparing_watchdog(pps));
    REQUIRE_FALSE(PrinterPrintStateTestAccess::fire_preparing_watchdog(pps));
}

TEST_CASE_METHOD(PreparingPausedFixture, "A paused print naming a different job supersedes ours",
                 "[print][preparing][1365]") {
    state().begin_preparing(PrintJobRef{"mine.gcode", "", ""});

    // Someone paused a different print out from under us. The claim is still
    // settled - it is just not ours - and Superseded keeps the heaters alone
    // while dropping the identity override.
    report("paused", "theirs.gcode");

    REQUIRE_FALSE(state().has_preparing_job());
    REQUIRE(state().last_preparing_exit() == PreparingExit::Superseded);
}

TEST_CASE_METHOD(PreparingPausedFixture, "Only a job the printer has taken settles the claim",
                 "[print][preparing][1365]") {
    // The widening stops at PRINTING and PAUSED. A terminal or idle state means
    // the printer is not holding a job, so our claim must survive - this is the
    // window begin_preparing() exists for.
    auto still_preparing = [&](const char* job_state) {
        state().retire_preparing(PreparingExit::Cancelled);
        state().begin_preparing(PrintJobRef{"mine.gcode", "", ""});
        report(job_state, "mine.gcode");
        return state().has_preparing_job();
    };

    REQUIRE(still_preparing("standby"));
    REQUIRE(still_preparing("complete"));
    REQUIRE(still_preparing("cancelled"));
    REQUIRE(still_preparing("error"));
}

TEST_CASE_METHOD(PreparingPausedFixture, "A printing report still confirms the claim",
                 "[print][preparing][1365]") {
    // Regression guard on the path that always worked.
    state().begin_preparing(PrintJobRef{"normal.gcode", "", ""});
    report("printing", "normal.gcode");

    REQUIRE_FALSE(state().has_preparing_job());
    REQUIRE(state().last_preparing_exit() == PreparingExit::Confirmed);
}
