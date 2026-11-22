// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "system/child_process.h"

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#ifdef __linux__
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

namespace helix {

#if !defined(ESP_PLATFORM)
namespace {

/// Close every fd from @p first up: close_range() where the kernel has it
/// (5.9+), else one close() per number below @p fallback_limit.
void close_fds_from(int first, int fallback_limit) {
#ifdef SYS_close_range
    if (syscall(SYS_close_range, static_cast<unsigned>(first), ~0U, 0U) == 0) {
        return;
    }
#endif
    for (int fd = first; fd < fallback_limit; ++fd) {
        close(fd);
    }
}

} // namespace
#endif

pid_t spawn_child(const char* file, const char* const* argv, char* const* envp, ChildIo io,
                  ChildOptions options) {
#if defined(ESP_PLATFORM)
    (void)file;
    (void)argv;
    (void)envp;
    (void)io;
    (void)options;
    errno = ENOSYS;
    return -1;
#else
    const long open_max = sysconf(_SC_OPEN_MAX);
    const int last_fd = open_max > 0 && open_max < 65536 ? static_cast<int>(open_max) : 65536;
    const pid_t parent = getpid();

    sigset_t all_signals;
    sigset_t old_mask;
    sigfillset(&all_signals);
    pthread_sigmask(SIG_SETMASK, &all_signals, &old_mask);
    const pid_t pid = fork();
    if (pid != 0) {
        const int fork_errno = errno;
        pthread_sigmask(SIG_SETMASK, &old_mask, nullptr);
        errno = fork_errno;
        return pid;
    }

    // The child: async-signal-safe calls only from here to exec.
    for (int sig = 1; sig < NSIG; ++sig) {
        struct sigaction current {};
        if (sigaction(sig, nullptr, &current) == 0 && current.sa_handler != SIG_IGN) {
            struct sigaction reset {};
            reset.sa_handler = SIG_DFL;
            sigaction(sig, &reset, nullptr);
        }
    }
#ifdef __linux__
    if (options.die_with_parent) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        // The app died before the signal was armed: it would never come.
        if (getppid() != parent) {
            _exit(127);
        }
    }
#endif
    (void)parent;
    sigprocmask(SIG_SETMASK, &old_mask, nullptr);

    // Every source moves above stderr first: one sitting on 0, 1 or 2 (an app
    // started with a standard stream closed hands those numbers out again)
    // would be overwritten by an earlier dup2, or dup2'd onto itself, which
    // leaves its close-on-exec flag set.
    int null_fd = open("/dev/null", O_RDWR);
    if (null_fd >= 0 && null_fd <= STDERR_FILENO) {
        null_fd = fcntl(null_fd, F_DUPFD, STDERR_FILENO + 1);
    }
    int sources[3] = {io.stdin_fd, io.stdout_fd, io.stderr_fd};
    for (int stream = 0; stream < 3; ++stream) {
        const int original = sources[stream];
        if (original < 0) {
            sources[stream] = null_fd;
        } else if (original <= STDERR_FILENO) {
            // Share one copy between streams wired to the same fd.
            int moved = -1;
            for (int earlier = 0; earlier < stream; ++earlier) {
                if (sources[earlier] >= 0 && (earlier == 0   ? io.stdin_fd
                                              : earlier == 1 ? io.stdout_fd
                                                             : io.stderr_fd) == original) {
                    moved = sources[earlier];
                }
            }
            sources[stream] = moved >= 0 ? moved : fcntl(original, F_DUPFD, STDERR_FILENO + 1);
        }
    }
    for (int stream = 0; stream < 3; ++stream) {
        if (sources[stream] >= 0) {
            dup2(sources[stream], stream);
        }
    }
    close_fds_from(STDERR_FILENO + 1, last_fd);
    if (options.nice != 0) {
        // Relative to the spawning thread's own value, which the child inherits.
        errno = 0;
        const int current = getpriority(PRIO_PROCESS, 0);
        if (errno == 0) {
            setpriority(PRIO_PROCESS, 0, current + options.nice);
        }
    }
    if (options.search_path) {
        execvp(file, const_cast<char* const*>(argv));
    } else if (envp) {
        execve(file, const_cast<char* const*>(argv), envp);
    } else {
        execv(file, const_cast<char* const*>(argv));
    }
    _exit(127);
#endif
}

int ChildWaitResult::exit_code() const {
    return reaped && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

namespace {

enum class Poll { Running, Reaped, Failed };

Poll poll_once(pid_t pid, ChildWaitResult& result) {
    for (;;) {
        const pid_t waited = ::waitpid(pid, &result.status, WNOHANG);
        if (waited == pid) {
            result.reaped = true;
            return Poll::Reaped;
        }
        if (waited == 0) {
            return Poll::Running;
        }
        if (errno == EINTR) {
            continue;
        }
        result.wait_errno = errno;
        return Poll::Failed;
    }
}

} // namespace

ChildWaitResult terminate_and_reap(pid_t pid, std::chrono::steady_clock::time_point deadline,
                                   std::chrono::milliseconds term_grace,
                                   std::chrono::milliseconds poll_interval,
                                   const std::function<bool()>& on_poll) {
    ChildWaitResult result;

    for (;;) {
        const bool keep_waiting = !on_poll || on_poll();
        const Poll state = poll_once(pid, result);
        if (state == Poll::Reaped) {
            result.outcome = ChildWaitOutcome::Exited;
            return result;
        }
        if (state == Poll::Failed) {
            result.outcome = ChildWaitOutcome::WaitFailed;
            return result;
        }
        if (!keep_waiting) {
            result.outcome = ChildWaitOutcome::Cancelled;
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            result.outcome = ChildWaitOutcome::TimedOut;
            break;
        }
        std::this_thread::sleep_for(poll_interval);
    }

    ::kill(pid, SIGTERM);
    const auto grace_end = std::chrono::steady_clock::now() + term_grace;
    for (;;) {
        if (on_poll) {
            on_poll();
        }
        const Poll state = poll_once(pid, result);
        if (state != Poll::Running) {
            return result;
        }
        if (std::chrono::steady_clock::now() >= grace_end) {
            break;
        }
        std::this_thread::sleep_for(poll_interval);
    }

    ::kill(pid, SIGKILL);
    result.killed = true;
    while (::waitpid(pid, &result.status, 0) < 0) {
        if (errno != EINTR) {
            result.wait_errno = errno;
            return result;
        }
    }
    result.reaped = true;
    return result;
}

} // namespace helix
