// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <functional>
#include <sys/types.h>

namespace helix {

/// Where a spawned child's standard streams go; -1 is /dev/null.
struct ChildIo {
    int stdin_fd = -1;
    int stdout_fd = -1;
    int stderr_fd = -1;
};

struct ChildOptions {
    /// Look @p file up on PATH (execvp, with the app's own environment)
    /// instead of running it as given with @p envp.
    bool search_path = false;
    /// Raise the child's nice value by this much (0 leaves it alone).
    int nice = 0;
    /// Kill the child when the thread that spawned it exits (Linux only), so
    /// a crashed app leaves no child running.
    bool die_with_parent = false;
};

/**
 * @brief fork() and exec @p file with @p argv, as a clean process
 *
 * Until exec the child is a copy of the app, signal handlers included, so a
 * signal that ends it early must not run the app's own handler in it: every
 * signal stays blocked across the fork, and the child resets each handled one
 * to its default before unblocking. It closes every inherited fd above stderr
 * and wires its streams per @p io. @p argv and @p envp (nullptr: the app's own
 * environment) must be built before the call: the child of a threaded process
 * may not allocate.
 *
 * @return the child's pid, or -1 with errno set when the fork failed (and
 *         always on a build that cannot spawn processes)
 */
pid_t spawn_child(const char* file, const char* const* argv, char* const* envp, ChildIo io,
                  ChildOptions options = {});

enum class ChildWaitOutcome {
    Exited,     ///< The child exited on its own before the deadline
    TimedOut,   ///< The deadline passed and the child was terminated
    Cancelled,  ///< on_poll asked to stop and the child was terminated
    WaitFailed, ///< waitpid failed; the child was not signalled
};

struct ChildWaitResult {
    ChildWaitOutcome outcome = ChildWaitOutcome::WaitFailed;
    int status = 0;      ///< waitpid status of the reaped child
    bool reaped = false; ///< status is valid; the pid must never be signalled again
    bool killed = false; ///< the child outlived the SIGTERM grace and got SIGKILL
    int wait_errno = 0;  ///< errno of the failed waitpid, when one failed

    /// Exit code of a child that exited, or -1 (signalled, not reaped).
    int exit_code() const;
};

/// Wait for child @p pid, then end it if it outlives @p deadline.
///
/// Polls waitpid(WNOHANG) every @p poll_interval until the child exits or the
/// deadline passes. Past the deadline it sends SIGTERM, keeps polling for up to
/// @p term_grace (returning as soon as the child is reaped), and sends SIGKILL
/// only to a child still unreaped, then reaps it. A pid is never signalled after
/// it was reaped or after waitpid failed, because it may already belong to an
/// unrelated process. EINTR is retried.
///
/// @p on_poll runs before every poll, the grace period's included, so a caller
/// can drain the child's output pipe. Returning false ends the wait early and
/// terminates the child the same way a timeout does; its return value is ignored
/// during the grace period.
ChildWaitResult terminate_and_reap(pid_t pid, std::chrono::steady_clock::time_point deadline,
                                   std::chrono::milliseconds term_grace,
                                   std::chrono::milliseconds poll_interval,
                                   const std::function<bool()>& on_poll = {});

} // namespace helix
