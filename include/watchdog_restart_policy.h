// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file watchdog_restart_policy.h
 * @brief Pure decision logic for helix-watchdog child restart handling.
 *
 * Header-only and dependency-free so it can be unit tested on hosts where
 * helix-watchdog itself is never built (mk/watchdog.mk only produces the
 * binary for DISPLAY_BACKEND=drm/fbdev, i.e. never on macOS/SDL).
 *
 * Two questions live here:
 *
 *  1. When exec/fork fails, is the failure PERMANENT (a broken or missing
 *     install; retrying reproduces it forever) or TRANSIENT (the machine was
 *     momentarily out of memory / the binary was momentarily unreadable;
 *     retrying succeeds)? The distinction matters because the two deserve
 *     opposite responses, and the kernel already tells us which is which via
 *     errno — the caller only has to stop throwing that away.
 *
 *  2. Given a classified failure and how many have piled up, should the
 *     supervisor retry soon, retry slowly, or surrender to the service manager?
 */

#ifndef HELIX_WATCHDOG_RESTART_POLICY_H
#define HELIX_WATCHDOG_RESTART_POLICY_H

#include <cerrno>

namespace helix::watchdog {

// =============================================================================
// Failure classification
// =============================================================================

/// How a failed execv()/fork() should be treated by the supervisor.
enum class ExecFailureClass {
    /// Not an exec/fork failure at all (the child ran and chose to exit non-zero).
    NONE,
    /// The install is broken: wrong path, no permission, not a directory, …
    /// Retrying reproduces the identical failure, so fail fast and loudly.
    PERMANENT,
    /// The system was momentarily unable to honour the exec: out of memory,
    /// process table full, image busy, I/O hiccup. The same binary execs fine
    /// once the pressure lifts, so retry patiently instead of surrendering.
    TRANSIENT,
};

// Reserved child exit codes used ONLY by the forked child to report why exec
// failed, plus the fork-failure path in the parent.
//
// The values must not collide with:
//   - 0                : clean exit
//   - 1                : helix-screen's own argument/startup failure exit
//   - 42               : RESTART_LOOP_EXIT_CODE, the watchdog's own surrender code
//   - 128..159         : the crash handler's exit(128 + signum) encoding
//
// 125/126/127 sit just below the signal-encoding range and follow the POSIX
// shell convention for exec problems (126 "found but not executable",
// 127 "not found"), so they read correctly in a shell-oriented log too.
inline constexpr int EXEC_FAILED_TRANSIENT_EXIT = 125;
inline constexpr int EXEC_FAILED_PERMANENT_EXIT = 126;
inline constexpr int EXEC_FAILED_UNCLASSIFIED_EXIT = 127;

/// Classify an errno set by a failed execv() or fork().
inline ExecFailureClass classify_exec_errno(int err) {
    switch (err) {
    // Resource pressure or a momentarily unusable image. All of these have
    // been observed to clear on their own: ENOEXEC in particular shows up on
    // low-RAM armv7 boards when the loader cannot fault in enough of the
    // program headers to validate the image.
    case ENOEXEC:
    case ENOMEM:
    case EAGAIN:
    case ETXTBSY:
    case EIO:
    case EBUSY:
        return ExecFailureClass::TRANSIENT;

    // The path itself is wrong or forbidden. No amount of waiting fixes it.
    case ENOENT:
    case EACCES:
    case ENOTDIR:
    case ELOOP:
    case EPERM:
        return ExecFailureClass::PERMANENT;

    default:
        return ExecFailureClass::NONE;
    }
}

/// Child exit code that reports @p err back to the supervising parent.
inline int exec_failure_exit_code(int err) {
    switch (classify_exec_errno(err)) {
    case ExecFailureClass::TRANSIENT:
        return EXEC_FAILED_TRANSIENT_EXIT;
    case ExecFailureClass::PERMANENT:
        return EXEC_FAILED_PERMANENT_EXIT;
    case ExecFailureClass::NONE:
    default:
        return EXEC_FAILED_UNCLASSIFIED_EXIT;
    }
}

/// Recover the classification the child encoded in its exit code.
/// Any other exit code means the child actually ran, so it is not an exec
/// failure at all.
inline ExecFailureClass classify_child_exit_code(int exit_code) {
    switch (exit_code) {
    case EXEC_FAILED_TRANSIENT_EXIT:
        return ExecFailureClass::TRANSIENT;
    case EXEC_FAILED_PERMANENT_EXIT:
        return ExecFailureClass::PERMANENT;
    default:
        return ExecFailureClass::NONE;
    }
}

/// Short, log-friendly name for a classification.
inline const char* exec_failure_class_name(ExecFailureClass cls) {
    switch (cls) {
    case ExecFailureClass::TRANSIENT:
        return "transient";
    case ExecFailureClass::PERMANENT:
        return "permanent";
    case ExecFailureClass::NONE:
    default:
        return "unclassified";
    }
}

// =============================================================================
// Restart pacing
// =============================================================================

/// First retry delay. Doubles on each consecutive failure.
inline constexpr int RESTART_BACKOFF_BASE_SEC = 3;
/// Ceiling for the doubling sequence: 3, 6, 12, 24, 48, 60, 60, …
inline constexpr int RESTART_BACKOFF_MAX_SEC = 60;

/// Consecutive TRANSIENT exec/fork failures tolerated at the backoff cadence
/// before dropping to the slow cooldown cadence. With the backoff above this
/// spans roughly 16 minutes, which comfortably outlasts an input-shaper FFT on
/// a low-RAM board (the pressure window that produces ENOEXEC in the first
/// place).
inline constexpr int TRANSIENT_MAX_FAILURES = 20;
/// Delay between cooldown rounds once the backoff budget is spent.
inline constexpr int TRANSIENT_COOLDOWN_SEC = 60;
/// Cooldown rounds before the supervisor finally surrenders. Each round starts
/// the backoff budget over, so a round is one cooldown plus a fresh ~16-minute
/// backoff sweep and ten of them span roughly three hours. Deliberately
/// generous: the alternative outcome is a black screen until the user reboots,
/// which is strictly worse than a supervisor that keeps quietly trying — but
/// still bounded, so a genuinely dead install eventually reaches the service
/// manager instead of retrying forever.
inline constexpr int TRANSIENT_MAX_COOLDOWN_ROUNDS = 10;

/// Delay before the Nth consecutive retry (N counted from 1).
inline int restart_backoff_seconds(int consecutive_failures) {
    if (consecutive_failures < 1) {
        consecutive_failures = 1;
    }
    long delay = RESTART_BACKOFF_BASE_SEC;
    for (int i = 1; i < consecutive_failures; ++i) {
        if (delay >= RESTART_BACKOFF_MAX_SEC) {
            break;
        }
        delay *= 2;
    }
    return delay > RESTART_BACKOFF_MAX_SEC ? RESTART_BACKOFF_MAX_SEC : static_cast<int>(delay);
}

/// What the supervisor should do after a failed child launch.
enum class RestartAction {
    /// Sleep for the (exponentially growing) backoff, then relaunch.
    RETRY_BACKOFF,
    /// Backoff budget spent: sleep a long fixed cooldown, reset the failure
    /// counter, and relaunch. Consumes one cooldown round.
    COOLDOWN_RETRY,
    /// Out of options — exit with RESTART_LOOP_EXIT_CODE so the service
    /// manager (or a human) sees the failure.
    GIVE_UP,
};

struct RestartDecision {
    RestartAction action = RestartAction::RETRY_BACKOFF;
    int delay_seconds = RESTART_BACKOFF_BASE_SEC;
};

/// Failure budget for non-transient failures: deliberate non-zero exits
/// ("another instance is already running", config validation, missing shared
/// library) and PERMANENT exec failures. These do not resolve by retrying, so
/// the budget stays small and the supervisor gives up quickly.
inline constexpr int RESTART_LOOP_MAX_FAILURES = 5;

/**
 * @brief Decide how to respond to a failed child launch.
 *
 * @param cls                   Classification of this failure.
 * @param consecutive_failures  For TRANSIENT: consecutive transient exec/fork
 *                              failures including this one. For everything
 *                              else: failures inside the rolling window,
 *                              including this one.
 * @param cooldown_rounds_used  Cooldown rounds already consumed (TRANSIENT only).
 */
inline RestartDecision decide_restart_action(ExecFailureClass cls, int consecutive_failures,
                                             int cooldown_rounds_used) {
    RestartDecision decision;

    if (cls == ExecFailureClass::TRANSIENT) {
        if (consecutive_failures <= TRANSIENT_MAX_FAILURES) {
            decision.action = RestartAction::RETRY_BACKOFF;
            decision.delay_seconds = restart_backoff_seconds(consecutive_failures);
            return decision;
        }
        if (cooldown_rounds_used < TRANSIENT_MAX_COOLDOWN_ROUNDS) {
            decision.action = RestartAction::COOLDOWN_RETRY;
            decision.delay_seconds = TRANSIENT_COOLDOWN_SEC;
            return decision;
        }
        decision.action = RestartAction::GIVE_UP;
        decision.delay_seconds = 0;
        return decision;
    }

    if (consecutive_failures > RESTART_LOOP_MAX_FAILURES) {
        decision.action = RestartAction::GIVE_UP;
        decision.delay_seconds = 0;
        return decision;
    }

    decision.action = RestartAction::RETRY_BACKOFF;
    decision.delay_seconds = restart_backoff_seconds(consecutive_failures);
    return decision;
}

// =============================================================================
// Clean exits
// =============================================================================

/**
 * @brief Whether a clean child exit should end the watchdog instead of respawning.
 *
 * An update replaces the install dir, and under systemd the root-privileged
 * unit refresh (helixscreen.service's ExecStartPre) only runs when the service
 * itself restarts. Respawning the child inside the running service skips it,
 * so a release's unit changes never reach the machine until a reboot
 * (prestonbrown/helixscreen#1713). Exiting lets Restart=always restart the
 * whole service.
 *
 * Without systemd nothing would bring the watchdog back, so every other clean
 * exit, and every clean exit off systemd, respawns in place.
 *
 * @param was_update_restart  The child left the update-restart marker.
 * @param invocation_id       getenv("INVOCATION_ID"): systemd sets it for
 *                            every process of a service, nothing else does.
 */
inline bool hand_clean_exit_to_service_manager(bool was_update_restart, const char* invocation_id) {
    return was_update_restart && invocation_id != nullptr && invocation_id[0] != '\0';
}

} // namespace helix::watchdog

#endif // HELIX_WATCHDOG_RESTART_POLICY_H
