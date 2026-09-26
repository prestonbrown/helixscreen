// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_watchdog_restart_policy.cpp
 * @brief Unit tests for helix-watchdog exec-failure classification and restart pacing.
 *
 * helix-watchdog itself is only built for DISPLAY_BACKEND=drm/fbdev (see
 * mk/watchdog.mk), so none of it links on macOS/SDL. The decision logic is
 * therefore factored into the header-only include/watchdog_restart_policy.h,
 * which is what these tests exercise. The surrounding fork/exec/waitpid
 * plumbing in src/helix_watchdog.cpp is not reachable from here and is covered
 * only by inspection plus the shell tests for the launcher's exit-code handling.
 *
 * Regression under test: on a low-RAM armv7 board an input-shaper FFT drove the
 * system deep enough into swap that execv() of an intact helix-screen binary
 * returned ENOEXEC. Every exec failure exited 127, the parent treated 127 as an
 * ordinary non-zero exit, and six of them inside the 60s window made the
 * supervisor exit 42 permanently — black screen until reboot.
 */

#include "watchdog_restart_policy.h"

#include <cerrno>
#include <set>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix::watchdog;

TEST_CASE("Resource-pressure exec errnos classify as transient", "[watchdog][restart]") {
    // Every one of these means "the system could not do it right now", not
    // "the install is broken". ENOEXEC is the one from the field report.
    CHECK(classify_exec_errno(ENOEXEC) == ExecFailureClass::TRANSIENT);
    CHECK(classify_exec_errno(ENOMEM) == ExecFailureClass::TRANSIENT);
    CHECK(classify_exec_errno(EAGAIN) == ExecFailureClass::TRANSIENT);
    CHECK(classify_exec_errno(ETXTBSY) == ExecFailureClass::TRANSIENT);
    CHECK(classify_exec_errno(EIO) == ExecFailureClass::TRANSIENT);
    CHECK(classify_exec_errno(EBUSY) == ExecFailureClass::TRANSIENT);
}

TEST_CASE("Broken-install exec errnos classify as permanent", "[watchdog][restart]") {
    CHECK(classify_exec_errno(ENOENT) == ExecFailureClass::PERMANENT);
    CHECK(classify_exec_errno(EACCES) == ExecFailureClass::PERMANENT);
    CHECK(classify_exec_errno(ENOTDIR) == ExecFailureClass::PERMANENT);
    CHECK(classify_exec_errno(ELOOP) == ExecFailureClass::PERMANENT);
    CHECK(classify_exec_errno(EPERM) == ExecFailureClass::PERMANENT);
}

TEST_CASE("Unknown exec errnos are not silently treated as transient", "[watchdog][restart]") {
    // An unrecognised errno must NOT inherit the generous transient budget —
    // that would turn every unmodelled failure into an hour of retrying.
    CHECK(classify_exec_errno(EINVAL) == ExecFailureClass::NONE);
    CHECK(classify_exec_errno(EMFILE) == ExecFailureClass::NONE);
    CHECK(classify_exec_errno(0) == ExecFailureClass::NONE);
}

TEST_CASE("Exec failure exit codes are distinct and collision-free", "[watchdog][restart]") {
    const int transient = exec_failure_exit_code(ENOEXEC);
    const int permanent = exec_failure_exit_code(ENOENT);
    const int unknown = exec_failure_exit_code(EINVAL);

    REQUIRE(transient == EXEC_FAILED_TRANSIENT_EXIT);
    REQUIRE(permanent == EXEC_FAILED_PERMANENT_EXIT);
    REQUIRE(unknown == EXEC_FAILED_UNCLASSIFIED_EXIT);

    std::set<int> codes{transient, permanent, unknown};
    CHECK(codes.size() == 3); // all three must be tellable apart by the parent

    for (int code : codes) {
        INFO("reserved exit code " << code);
        CHECK(code != 0);  // 0 means "clean exit, restart silently"
        CHECK(code != 1);  // helix-screen's own startup-failure exit
        CHECK(code != 42); // RESTART_LOOP_EXIT_CODE — the watchdog's surrender signal
        // 128..159 is the crash handler's exit(128 + signum) encoding; a code in
        // that range would be re-read as a signal and pop the recovery dialog.
        CHECK_FALSE((code >= 128 && code <= 159));
        CHECK(code > 0);
        CHECK(code < 128);
    }
}

TEST_CASE("Parent recovers the classification from the child exit code", "[watchdog][restart]") {
    // Round-trip: what the forked child encodes is what the parent decodes.
    const int errnos[] = {ENOEXEC, ENOMEM, EAGAIN,  ETXTBSY, EIO,  EBUSY,
                          ENOENT,  EACCES, ENOTDIR, ELOOP,   EPERM};
    for (int err : errnos) {
        INFO("errno " << err);
        CHECK(classify_child_exit_code(exec_failure_exit_code(err)) == classify_exec_errno(err));
    }

    // A child that actually ran and exited non-zero is not an exec failure.
    CHECK(classify_child_exit_code(0) == ExecFailureClass::NONE);
    CHECK(classify_child_exit_code(1) == ExecFailureClass::NONE);
    CHECK(classify_child_exit_code(127) == ExecFailureClass::NONE);
    CHECK(classify_child_exit_code(139) == ExecFailureClass::NONE);
}

TEST_CASE("Restart backoff grows exponentially and is capped", "[watchdog][restart]") {
    CHECK(restart_backoff_seconds(1) == 3);
    CHECK(restart_backoff_seconds(2) == 6);
    CHECK(restart_backoff_seconds(3) == 12);
    CHECK(restart_backoff_seconds(4) == 24);
    CHECK(restart_backoff_seconds(5) == 48);
    CHECK(restart_backoff_seconds(6) == RESTART_BACKOFF_MAX_SEC);
    CHECK(restart_backoff_seconds(50) == RESTART_BACKOFF_MAX_SEC);

    // Never regresses, never runs away, never returns a zero/negative sleep
    // (a zero sleep would busy-loop the supervisor).
    int prev = 0;
    for (int n = 0; n <= 100; ++n) {
        int d = restart_backoff_seconds(n);
        INFO("attempt " << n);
        CHECK(d > 0);
        CHECK(d <= RESTART_BACKOFF_MAX_SEC);
        CHECK(d >= prev);
        prev = d;
    }
}

TEST_CASE("Permanent and unclassified failures still give up quickly", "[watchdog][restart]") {
    for (auto cls : {ExecFailureClass::PERMANENT, ExecFailureClass::NONE}) {
        for (int n = 1; n <= RESTART_LOOP_MAX_FAILURES; ++n) {
            INFO("failure " << n);
            CHECK(decide_restart_action(cls, n, 0).action == RestartAction::RETRY_BACKOFF);
        }
        CHECK(decide_restart_action(cls, RESTART_LOOP_MAX_FAILURES + 1, 0).action ==
              RestartAction::GIVE_UP);
        // No cooldown reprieve for these: retrying provably cannot help.
        CHECK(decide_restart_action(cls, RESTART_LOOP_MAX_FAILURES + 1, 5).action ==
              RestartAction::GIVE_UP);
    }
}

TEST_CASE("Transient failures do not spend the fast give-up budget", "[watchdog][restart]") {
    // This is the exact incident: six consecutive ENOEXEC launches. Under the
    // old policy the sixth returned RESTART_LOOP_EXIT_CODE and the screen
    // stayed black until the user power-cycled the printer.
    const auto cls = classify_exec_errno(ENOEXEC);
    for (int n = 1; n <= RESTART_LOOP_MAX_FAILURES + 1; ++n) {
        INFO("transient failure " << n);
        CHECK(decide_restart_action(cls, n, 0).action == RestartAction::RETRY_BACKOFF);
    }
}

TEST_CASE("Transient budget rolls into cooldown, not surrender", "[watchdog][restart]") {
    const auto cls = ExecFailureClass::TRANSIENT;

    // Last attempt inside the backoff budget.
    CHECK(decide_restart_action(cls, TRANSIENT_MAX_FAILURES, 0).action ==
          RestartAction::RETRY_BACKOFF);

    // One past it: slow down, do not surrender.
    RestartDecision cooldown = decide_restart_action(cls, TRANSIENT_MAX_FAILURES + 1, 0);
    CHECK(cooldown.action == RestartAction::COOLDOWN_RETRY);
    CHECK(cooldown.delay_seconds == TRANSIENT_COOLDOWN_SEC);

    // Cooldown rounds are bounded but generous.
    CHECK(decide_restart_action(cls, TRANSIENT_MAX_FAILURES + 1, TRANSIENT_MAX_COOLDOWN_ROUNDS - 1)
              .action == RestartAction::COOLDOWN_RETRY);
    CHECK(decide_restart_action(cls, TRANSIENT_MAX_FAILURES + 1, TRANSIENT_MAX_COOLDOWN_ROUNDS)
              .action == RestartAction::GIVE_UP);
}

TEST_CASE("Transient policy outlasts a long calibration", "[watchdog][restart]") {
    // The trigger was a SHAPER_CALIBRATE pass on both axes; a multi-minute FFT
    // on a slower board must be survivable, so sum the wall time the policy
    // will keep trying for before it ever returns GIVE_UP.
    const auto cls = ExecFailureClass::TRANSIENT;
    long total = 0;
    int failures = 0;
    int cooldowns = 0;

    for (int i = 0; i < 5000; ++i) {
        RestartDecision d = decide_restart_action(cls, failures + 1, cooldowns);
        if (d.action == RestartAction::GIVE_UP) {
            break;
        }
        total += d.delay_seconds;
        if (d.action == RestartAction::COOLDOWN_RETRY) {
            ++cooldowns;
            failures = 0; // cooldown clears the failure window
        } else {
            ++failures;
        }
    }

    INFO("total retry window: " << total << "s across " << cooldowns << " cooldown rounds");
    CHECK(cooldowns == TRANSIENT_MAX_COOLDOWN_ROUNDS);
    CHECK(total > 60 * 60);      // at least an hour of patience
    CHECK(total < 12 * 60 * 60); // but still bounded — the loop must terminate
}

TEST_CASE("Class names are distinct and log-safe", "[watchdog][restart]") {
    CHECK(std::string(exec_failure_class_name(ExecFailureClass::TRANSIENT)) == "transient");
    CHECK(std::string(exec_failure_class_name(ExecFailureClass::PERMANENT)) == "permanent");
    CHECK(std::string(exec_failure_class_name(ExecFailureClass::NONE)) == "unclassified");
}

TEST_CASE("Only an update restart under a restarting service is handed off",
          "[watchdog][restart]") {
    // helixscreen.service restarts on exit and re-runs ExecStartPre (#1713).
    CHECK(hand_clean_exit_to_service_manager(true, "1"));

    // Nothing declared a restarting service: exiting leaves a dead screen.
    CHECK_FALSE(hand_clean_exit_to_service_manager(true, nullptr));
    CHECK_FALSE(hand_clean_exit_to_service_manager(true, ""));

    // An ordinary clean exit (Restart App, SIGTERM fast exit) respawns in place.
    CHECK_FALSE(hand_clean_exit_to_service_manager(false, "1"));
    CHECK_FALSE(hand_clean_exit_to_service_manager(false, nullptr));
}
