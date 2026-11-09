// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

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

/// The last line of @p output with any text in it, color escapes removed.
std::string last_output_line(const std::string& output);

} // namespace helix
