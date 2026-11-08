// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file watchdog_isolated_dialog.h
 * @brief Runs helix-watchdog's recovery UI in a process of its own.
 *
 * The watchdog is the one process that must survive to relaunch helix-screen.
 * A forked child owns the LVGL state, the framebuffer and the touch device, and
 * reports the user's choice as its exit code, so a crash in the UI costs the
 * dialog and never the supervisor.
 */

#ifndef HELIX_WATCHDOG_ISOLATED_DIALOG_H
#define HELIX_WATCHDOG_ISOLATED_DIALOG_H

#include <cerrno>
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace helix::watchdog {

enum class IsolatedDialogOutcome {
    COMPLETED,   ///< The child exited by itself; exit_code holds its answer.
    FORK_FAILED, ///< No child ran; os_errno holds the fork() error.
    CHILD_DIED,  ///< The child was killed; term_signal names the signal.
    INTERRUPTED, ///< The supervisor was asked to quit and killed the child.
};

struct IsolatedDialogResult {
    IsolatedDialogOutcome outcome = IsolatedDialogOutcome::FORK_FAILED;
    int exit_code = 0;
    int term_signal = 0;
    int os_errno = 0;
};

/**
 * @brief Run @p body in a forked child and wait for it.
 *
 * The child leaves through _exit(), so static destructors and buffered parent
 * state never run in it. @p quit is re-checked whenever a signal interrupts the
 * wait; once set, the child is killed and reaped before this returns.
 *
 * @param body Callable returning the child's exit code (0..255).
 * @param quit Flag set by the supervisor's signal handler.
 */
template <typename Body>
IsolatedDialogResult run_isolated_dialog(Body body, const volatile sig_atomic_t& quit) {
    IsolatedDialogResult result;

    const pid_t child = fork();
    if (child < 0) {
        result.outcome = IsolatedDialogOutcome::FORK_FAILED;
        result.os_errno = errno;
        return result;
    }
    if (child == 0) {
        _exit(body());
    }

    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            result.outcome = IsolatedDialogOutcome::CHILD_DIED;
            result.os_errno = errno;
            return result;
        }
        if (quit) {
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
            }
            result.outcome = IsolatedDialogOutcome::INTERRUPTED;
            return result;
        }
    }

    if (WIFEXITED(status)) {
        result.outcome = IsolatedDialogOutcome::COMPLETED;
        result.exit_code = WEXITSTATUS(status);
    } else {
        result.outcome = IsolatedDialogOutcome::CHILD_DIED;
        result.term_signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
    }
    return result;
}

} // namespace helix::watchdog

#endif // HELIX_WATCHDOG_ISOLATED_DIALOG_H
