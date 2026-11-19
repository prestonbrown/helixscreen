// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace helix {

/// What one command did.
struct CommandResult {
    bool started = false;   ///< the command was spawned
    bool timed_out = false; ///< it outlived its timeout and was stopped
    int exit_code = -1;     ///< its exit status; -1 when it did not exit on its own
    std::string output;     ///< stdout and stderr together, capped
    std::string error;      ///< why it could not be spawned or waited for; empty otherwise

    bool succeeded() const {
        return started && !timed_out && exit_code == 0;
    }
};

/// Runs @p argv (argv[0] looked up on PATH) for up to @p timeout_seconds.
/// Blocking. Code that runs commands takes one, so tests hand it a fake.
using ProcessRunner =
    std::function<CommandResult(const std::vector<std::string>& argv, int timeout_seconds)>;

/// The real runner: forks /bin/sh to exec @p argv, never interpreting its words,
/// and captures the output while it waits. BLOCKING: never on the LVGL thread.
CommandResult run_process(const std::vector<std::string>& argv, int timeout_seconds);

/// run_process() that also ends the running command, as at a timeout, once
/// @p cancel turns true. @p cancel must outlive every call through the runner.
ProcessRunner cancellable_process_runner(const std::atomic<bool>& cancel);

/// @p run for a command that must not be stopped part way: once @p cancel is
/// true it refuses to start one (started stays false), and a command it has
/// started runs to its own timeout. @p cancel must outlive every call.
ProcessRunner start_unless_cancelled(const std::atomic<bool>& cancel, ProcessRunner run);

/// The last line of @p output with any text in it, color escapes removed.
std::string last_output_line(const std::string& output);

/// Log @p what's output line by line: the command's own messages are the only
/// record of why it failed, so a failure logs them at warn; a success keeps
/// them at debug, except its own [WARN] lines.
void log_command_output(const char* what, const std::string& output, bool succeeded);

} // namespace helix
