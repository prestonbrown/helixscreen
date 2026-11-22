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
#endif

namespace helix {

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
    }
#endif
    sigprocmask(SIG_SETMASK, &old_mask, nullptr);

    const int null_fd = open("/dev/null", O_RDWR);
    const int targets[3] = {io.stdin_fd, io.stdout_fd, io.stderr_fd};
    for (int stream = 0; stream < 3; ++stream) {
        const int from = targets[stream] >= 0 ? targets[stream] : null_fd;
        if (from >= 0) {
            dup2(from, stream);
        }
    }
    for (int fd = STDERR_FILENO + 1; fd < last_fd; ++fd) {
        close(fd);
    }
    if (options.nice != 0) {
        setpriority(PRIO_PROCESS, 0, options.nice);
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
