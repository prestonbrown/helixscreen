// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Run one hidden case of the test binary in a fresh process of its own.

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ; // the libc environment, handed to the child process

namespace helix::test {

struct ChildRun {
    int rc = -1;        ///< 0 when every assertion passed, 128 + signal, -1 on a hang
    std::string output; ///< the child's stdout and stderr
};

/// Runs the hidden case @p name in a fresh helix-tests process that has
/// @p child_env set to "1" and nothing else changed. The case skips when the
/// variable is absent, so the hidden-set run (make test-hidden) takes nothing
/// from it.
///
/// A fresh process is the only place for anything this process's own Catch2
/// session owns: a failing assertion's report (a failure fails whatever case
/// holds it), or LVGL torn down (this process never calls lv_deinit(), and a
/// forked child cannot either, because lv_deinit() joins draw threads the
/// child lacks).
inline ChildRun run_case_in_fresh_process(const char* name, const char* child_env) {
    ChildRun run;
    // Built before fork: the child only redirects and execs.
    std::vector<std::string> env_strings;
    for (char** e = environ; *e != nullptr; ++e) {
        env_strings.emplace_back(*e);
    }
    env_strings.emplace_back(std::string(child_env) + "=1");
    std::vector<char*> envp;
    for (auto& e : env_strings) {
        envp.push_back(e.data());
    }
    envp.push_back(nullptr);
    char exe[] = "/proc/self/exe";
    char argv0[] = "helix-tests";
    std::string filter(name);
    char* argv[] = {argv0, filter.data(), nullptr};
    char out_path[] = "/tmp/helix-fresh-child-XXXXXX";
    const int out_fd = mkstemp(out_path);
    if (out_fd < 0) {
        run.output = "mkstemp failed";
        return run;
    }

    const pid_t pid = fork();
    if (pid == 0) {
        dup2(out_fd, STDOUT_FILENO);
        dup2(out_fd, STDERR_FILENO);
        execve(exe, argv, envp.data());
        _exit(127);
    }
    if (pid > 0) {
        int status = 0;
        bool done = false;
        for (int waited_ms = 0; waited_ms < 30000 && !done; waited_ms += 10) {
            if (waitpid(pid, &status, WNOHANG) == pid) {
                done = true;
                run.rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        if (!done) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
        }
    }
    std::ifstream in(out_path);
    run.output.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    close(out_fd);
    unlink(out_path);
    return run;
}

} // namespace helix::test
