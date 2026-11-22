// SPDX-License-Identifier: GPL-3.0-or-later
#include "system/child_process.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ChildWaitOutcome;
using helix::terminate_and_reap;
using namespace std::chrono_literals;

namespace {

/// Fork and exec /bin/sleep @p seconds. With @p ignore_term the child ignores
/// SIGTERM, a disposition that survives exec.
///
/// SIGTERM stays blocked across the fork: a child that has not reached exec
/// yet still carries Catch2's fatal-signal handler, which would report the
/// test's own SIGTERM as a crash and outlive the grace period.
pid_t spawn_sleep(const char* seconds, bool ignore_term = false) {
    sigset_t term, saved;
    sigemptyset(&term);
    sigaddset(&term, SIGTERM);
    ::sigprocmask(SIG_BLOCK, &term, &saved);
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::signal(SIGTERM, ignore_term ? SIG_IGN : SIG_DFL);
        ::sigprocmask(SIG_SETMASK, &saved, nullptr);
        ::execl("/bin/sleep", "sleep", seconds, static_cast<char*>(nullptr));
        _exit(127);
    }
    ::sigprocmask(SIG_SETMASK, &saved, nullptr);
    return pid;
}

pid_t spawn_exit(int code) {
    const pid_t pid = ::fork();
    if (pid == 0) {
        _exit(code);
    }
    return pid;
}

auto from_now(std::chrono::milliseconds ms) {
    return std::chrono::steady_clock::now() + ms;
}

} // namespace

TEST_CASE("terminate_and_reap returns a child that exits on its own", "[child_process]") {
    const pid_t pid = spawn_exit(3);
    REQUIRE(pid > 0);
    int polls = 0;
    const auto r = terminate_and_reap(pid, from_now(5000ms), 500ms, 5ms, [&] {
        ++polls;
        return true;
    });
    CHECK(r.outcome == ChildWaitOutcome::Exited);
    CHECK(r.reaped);
    CHECK_FALSE(r.killed);
    CHECK(r.exit_code() == 3);
    CHECK(polls >= 1);
}

TEST_CASE("terminate_and_reap ends a child past its deadline with SIGTERM", "[child_process]") {
    const pid_t pid = spawn_sleep("30");
    REQUIRE(pid > 0);
    const auto start = std::chrono::steady_clock::now();
    const auto r = terminate_and_reap(pid, from_now(50ms), 10000ms, 5ms);
    const auto took = std::chrono::steady_clock::now() - start;

    CHECK(r.outcome == ChildWaitOutcome::TimedOut);
    CHECK(r.reaped);
    CHECK_FALSE(r.killed);
    CHECK(WIFSIGNALED(r.status));
    CHECK(WTERMSIG(r.status) == SIGTERM);
    CHECK(r.exit_code() == -1);
    // The grace ends at the reap, not at its 10s limit.
    CHECK(took < 5000ms);
}

TEST_CASE("terminate_and_reap SIGKILLs a child that ignores SIGTERM", "[child_process]") {
    const pid_t pid = spawn_sleep("30", true);
    REQUIRE(pid > 0);
    const auto r = terminate_and_reap(pid, from_now(50ms), 100ms, 5ms);

    CHECK(r.outcome == ChildWaitOutcome::TimedOut);
    CHECK(r.reaped);
    CHECK(r.killed);
    CHECK(WIFSIGNALED(r.status));
    CHECK(WTERMSIG(r.status) == SIGKILL);
}

TEST_CASE("terminate_and_reap ends the child when on_poll cancels", "[child_process]") {
    const pid_t pid = spawn_sleep("30");
    REQUIRE(pid > 0);
    int polls = 0;
    const auto r = terminate_and_reap(pid, from_now(30000ms), 10000ms, 5ms, [&] {
        ++polls;
        return polls < 3;
    });

    CHECK(r.outcome == ChildWaitOutcome::Cancelled);
    CHECK(r.reaped);
    CHECK_FALSE(r.killed);
    CHECK(WTERMSIG(r.status) == SIGTERM);
}

TEST_CASE("terminate_and_reap signals nothing once waitpid fails", "[child_process]") {
    // A pid this process already reaped: waitpid reports ECHILD, and the pid may
    // now name an unrelated process, so it must not be signalled.
    const pid_t pid = spawn_exit(0);
    REQUIRE(pid > 0);
    int status = 0;
    REQUIRE(::waitpid(pid, &status, 0) == pid);

    const auto r = terminate_and_reap(pid, from_now(0ms), 0ms, 1ms);
    CHECK(r.outcome == ChildWaitOutcome::WaitFailed);
    CHECK_FALSE(r.reaped);
    CHECK_FALSE(r.killed);
    CHECK(r.wait_errno == ECHILD);
    CHECK(r.exit_code() == -1);
}

// --- spawn_child ---

namespace {

/// Run @p argv (looked up on PATH) through spawn_child with stdout on a pipe;
/// its output.
std::string spawn_argv_and_read(const char* const* argv) {
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    helix::ChildIo io;
    io.stdout_fd = fds[1];
    helix::ChildOptions options;
    options.search_path = true;
    const pid_t pid = helix::spawn_child(argv[0], argv, nullptr, io, options);
    ::close(fds[1]);
    REQUIRE(pid > 0);
    std::string out;
    char buf[256];
    ssize_t n;
    while ((n = ::read(fds[0], buf, sizeof(buf))) > 0) {
        out.append(buf, static_cast<size_t>(n));
    }
    ::close(fds[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    return out;
}

/// Run `sh -c @p script` through spawn_child; its output.
std::string spawn_and_read(const char* script) {
    const char* argv[] = {"sh", "-c", script, nullptr};
    return spawn_argv_and_read(argv);
}

std::string blocked_mask_line(const std::string& status_text) {
    const auto at = status_text.find("SigBlk:");
    if (at == std::string::npos) {
        return {};
    }
    return status_text.substr(at, status_text.find('\n', at) - at);
}

} // namespace

TEST_CASE("spawn_child wires stdout to the given fd", "[child_process]") {
    CHECK(spawn_and_read("echo hello") == "hello\n");
}

TEST_CASE("spawn_child closes every inherited fd above stderr", "[child_process]") {
    // A descriptor the app holds without CLOEXEC (a socket, say), numbered
    // above anything ls opens for itself.
    const int opened = ::open("/dev/null", O_RDONLY);
    REQUIRE(opened > 2);
    const int leaked = ::dup2(opened, 77);
    ::close(opened);
    REQUIRE(leaked == 77);
    const std::string fds = spawn_and_read("ls /proc/self/fd");
    ::close(leaked);
    // ls sees 0, 1, 2 and the directory it is listing, and nothing else.
    std::vector<std::string> names;
    size_t pos = 0;
    while (pos < fds.size()) {
        const auto end = fds.find('\n', pos);
        names.push_back(fds.substr(pos, end - pos));
        pos = end + 1;
    }
    CHECK(names.size() == 4);
    CHECK(std::find(names.begin(), names.end(), std::to_string(leaked)) == names.end());
}

TEST_CASE("spawn_child leaves both signal masks as the parent's", "[child_process]") {
    std::ifstream self("/proc/self/status");
    std::stringstream before;
    before << self.rdbuf();
    // The child's own mask after exec, read by the program it runs (no shell:
    // a shell clears its mask when it starts).
    const char* grep[] = {"grep", "SigBlk", "/proc/self/status", nullptr};
    const std::string child = spawn_argv_and_read(grep);
    std::ifstream again("/proc/self/status");
    std::stringstream after;
    after << again.rdbuf();
    // The worker thread's mask may differ from the process's; compare this thread's.
    CHECK(blocked_mask_line(after.str()) == blocked_mask_line(before.str()));
    CHECK(child.substr(0, child.find('\n')) == blocked_mask_line(before.str()));
}

TEST_CASE("spawn_child hands execve the environment it is given", "[child_process]") {
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    helix::ChildIo io;
    io.stdout_fd = fds[1];
    const char* argv[] = {"sh", "-c", "echo \"$HELIX_SPAWN_TEST\"", nullptr};
    char var[] = "HELIX_SPAWN_TEST=42";
    char* envp[] = {var, nullptr};
    const pid_t pid = helix::spawn_child("/bin/sh", argv, envp, io);
    ::close(fds[1]);
    REQUIRE(pid > 0);
    char buf[16] = {};
    const ssize_t n = ::read(fds[0], buf, sizeof(buf) - 1);
    ::close(fds[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    CHECK(std::string(buf, n > 0 ? static_cast<size_t>(n) : 0) == "42\n");
}

TEST_CASE("spawn_child gives unwired streams /dev/null", "[child_process]") {
    // stdin reads empty at once (no hang on the test's own stdin), and stderr
    // goes nowhere near the stdout pipe.
    CHECK(spawn_and_read("cat; echo to-stderr >&2; echo done") == "done\n");
}

TEST_CASE("spawn_child: a program that cannot be run exits 127", "[child_process]") {
    const char* argv[] = {"helix-no-such-program-xyz", nullptr};
    helix::ChildOptions options;
    options.search_path = true;
    const pid_t pid = helix::spawn_child(argv[0], argv, nullptr, {}, options);
    REQUIRE(pid > 0);
    int status = 0;
    ::waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 127);
}

TEST_CASE("spawn_child raises nice relative to the spawning thread", "[child_process]") {
    // A worker niced above the process that asks for +3 gives a child 3 above
    // the worker, never 3 above zero. The runner may already be niced: every
    // value is relative to what this thread starts at.
    std::string child_nice;
    int worker_nice = 0;
    std::thread worker([&] {
        const auto tid = static_cast<id_t>(::syscall(SYS_gettid));
        ::setpriority(PRIO_PROCESS, tid, ::getpriority(PRIO_PROCESS, tid) + 2);
        worker_nice = ::getpriority(PRIO_PROCESS, tid);
        int fds[2];
        if (::pipe(fds) != 0) {
            return;
        }
        helix::ChildIo io;
        io.stdout_fd = fds[1];
        helix::ChildOptions options;
        options.nice = 3;
        const char* argv[] = {"sh", "-c", "cut -d' ' -f19 /proc/self/stat", nullptr};
        const pid_t pid = helix::spawn_child("/bin/sh", argv, nullptr, io, options);
        ::close(fds[1]);
        char buf[16] = {};
        const ssize_t n = pid > 0 ? ::read(fds[0], buf, sizeof(buf) - 1) : 0;
        ::close(fds[0]);
        int status = 0;
        if (pid > 0) {
            ::waitpid(pid, &status, 0);
        }
        child_nice.assign(buf, n > 0 ? static_cast<size_t>(n) : 0);
    });
    worker.join();
    CHECK(child_nice == std::to_string(std::min(worker_nice + 3, 19)) + "\n");
}

TEST_CASE("spawn_child: die_with_parent ends the child with its spawning thread",
          "[child_process]") {
    pid_t child = -1;
    std::thread worker([&] {
        const char* argv[] = {"sleep", "30", nullptr};
        helix::ChildOptions options;
        options.search_path = true;
        options.die_with_parent = true;
        child = helix::spawn_child(argv[0], argv, nullptr, {}, options);
        // Long enough for the child to arm the signal; a thread that exits
        // first is a process-death race the child checks with getppid().
        std::this_thread::sleep_for(300ms);
    });
    worker.join();
    REQUIRE(child > 0);
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    pid_t got = 0;
    while (got == 0 && std::chrono::steady_clock::now() < deadline) {
        got = ::waitpid(child, &status, WNOHANG);
        std::this_thread::sleep_for(10ms);
    }
    if (got == 0) {
        ::kill(child, SIGKILL);
        ::waitpid(child, &status, 0);
    }
    CHECK(got == child);
    CHECK(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGKILL);
}

TEST_CASE("spawn_child wires a pipe that landed on a standard stream's number", "[child_process]") {
    // An app started with stdin closed gets fd 0 back for its next pipe.
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    const int saved_stdin = ::dup(STDIN_FILENO);
    REQUIRE(saved_stdin >= 0);
    ::close(STDIN_FILENO);
    const int write_end = ::dup(fds[1]); // the lowest free number: 0
    ::close(fds[1]);
    helix::ChildIo io;
    io.stdout_fd = write_end;
    const char* argv[] = {"sh", "-c", "echo hello", nullptr};
    const pid_t pid = helix::spawn_child("/bin/sh", argv, nullptr, io);
    ::close(write_end);
    ::dup2(saved_stdin, STDIN_FILENO);
    ::close(saved_stdin);
    REQUIRE(write_end == 0);
    REQUIRE(pid > 0);
    std::string out;
    char buf[64];
    ssize_t n;
    while ((n = ::read(fds[0], buf, sizeof(buf))) > 0) {
        out.append(buf, static_cast<size_t>(n));
    }
    ::close(fds[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    CHECK(out == "hello\n");
}
