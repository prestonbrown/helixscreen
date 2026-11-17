// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <thread>
#include <vector>

namespace helix::terminal {

/// The shell to run: @p shell_env ($SHELL) when it names an executable, else /bin/sh.
/// @p is_executable is the probe, so a test can answer it.
std::string choose_shell(const char* shell_env,
                         const std::function<bool(const std::string&)>& is_executable);

/// The session id in one /proc/<pid>/stat line, or -1 when the line does not parse.
/// The command name in parentheses may itself hold spaces and parentheses.
pid_t session_from_proc_stat(std::string_view stat);

/// Every process under @p proc_root (normally "/proc") whose session is @p sid.
std::vector<pid_t> session_members(const std::string& proc_root, pid_t sid);

/// Output from the child waiting to be drawn, between the reader thread and the main
/// thread. The reader appends and blocks while it holds @p cap bytes or more, so a
/// child that writes faster than the screen draws is held back by its pty instead of
/// growing this buffer. Thread-safe.
class OutputBuffer {
  public:
    explicit OutputBuffer(std::size_t cap) : cap_(cap) {}

    /// Reader side. Waits while the buffer is full; returns false without appending
    /// once close() was called. Returns through @p was_empty whether the buffer held
    /// nothing before this append: the one append that needs a wakeup sent.
    bool append(std::string_view bytes, bool& was_empty);
    /// Main side. Everything buffered, emptying it and waking a blocked reader.
    std::string take();
    /// Unblocks the reader for good; later appends are refused.
    void close();
    std::size_t size() const;

  private:
    mutable std::mutex mutex_;
    std::condition_variable space_;
    std::string bytes_;
    std::size_t cap_;
    bool closed_ = false;
};

/// A shell on a pseudo-terminal, read on its own thread.
///
/// start() forks the shell as a session leader with the pty's slave side as its
/// controlling terminal. The reader thread moves output into an OutputBuffer and calls
/// the wake callback, on the reader thread, when the buffer goes from empty to
/// non-empty and once more when the child hangs up; the owner drains on its own
/// thread. stop() joins the reader, hangs the pty up, reaps the shell (SIGKILL if it
/// outlives a short grace), then SIGKILLs whatever is still in the shell's session, a
/// job that ignored the hangup included. Only a process that left the session on
/// purpose (setsid, a daemon) outlives the terminal. The destructor
/// calls stop().
class PtySession {
  public:
    using WakeFn = std::function<void()>;

    static constexpr std::size_t kMaxBufferedOutput = 64 * 1024;

    PtySession() = default;
    ~PtySession();
    PtySession(const PtySession&) = delete;
    PtySession& operator=(const PtySession&) = delete;

    /// Run @p argv (argv[0] a path) on a new pty of @p rows x @p cols, with @p env
    /// entries ("NAME=value") added to the inherited environment. False with a reason
    /// in @p error when the pty or the fork failed.
    bool start(const std::vector<std::string>& argv, const std::vector<std::string>& env, int rows,
               int cols, WakeFn wake, std::string& error);

    /// Write @p bytes to the child's terminal. False once the child is gone.
    bool write(std::string_view bytes);
    /// Tell the child its terminal is now @p rows x @p cols (SIGWINCH).
    void resize(int rows, int cols);
    /// Output waiting since the last call.
    std::string take_output() {
        return output_.take();
    }
    /// The child hung up (exited, or closed its terminal). Its exit is reaped by stop().
    bool hung_up() const {
        return hung_up_.load(std::memory_order_acquire);
    }
    bool running() const {
        return pid_ > 0;
    }
    pid_t pid() const {
        return pid_;
    }

    /// End the child and the reader. Safe to call twice and on a session never started.
    /// Returns the child's exit code, or -1 when it was killed or never ran.
    int stop();

  private:
    void read_loop();

    int master_fd_ = -1;
    int wake_pipe_[2] = {-1, -1};
    pid_t pid_ = -1;
    std::thread reader_;
    WakeFn wake_;
    OutputBuffer output_{kMaxBufferedOutput};
    std::atomic<bool> hung_up_{false};
    std::atomic<bool> stopping_{false};
};

} // namespace helix::terminal
