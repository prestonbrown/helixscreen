// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "system/pty_session.h"

#include "helix_features.h"

#if HELIX_HAS_HOST_TERMINAL

#include "helix_thread.h"
#include "system/child_process.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <utility>

// NAMESPACE_OK: the POSIX process environment is a global
extern char** environ;

namespace helix::terminal {

namespace {

/// Highest descriptor the child closes before exec. Anything the app opened without
/// O_CLOEXEC (the display, sockets) must not reach the shell.
constexpr int kMaxInheritedFd = 4096;
/// How long a hung-up shell gets to exit before SIGKILL.
constexpr auto kExitGrace = std::chrono::milliseconds(250);
constexpr auto kReapPoll = std::chrono::milliseconds(10);
constexpr std::size_t kReadChunk = 4096;

void set_winsize(int fd, int rows, int cols) {
    struct winsize ws {};
    ws.ws_row = static_cast<unsigned short>(std::max(rows, 1));
    ws.ws_col = static_cast<unsigned short>(std::max(cols, 1));
    ::ioctl(fd, TIOCSWINSZ, &ws);
}

void close_fd(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

/// The name part of an environment entry, "NAME" of "NAME=value".
std::string_view env_name(std::string_view entry) {
    return entry.substr(0, entry.find('='));
}

} // namespace

pid_t session_from_proc_stat(std::string_view stat) {
    // pid (comm) state ppid pgrp session ...
    const auto close = stat.rfind(')');
    if (close == std::string_view::npos) {
        return -1;
    }
    std::string_view rest = stat.substr(close + 1);
    for (int field = 0; field < 4; ++field) {
        const auto start = rest.find_first_not_of(' ');
        if (start == std::string_view::npos) {
            return -1;
        }
        rest = rest.substr(start);
        if (field == 3) {
            break;
        }
        const auto end = rest.find(' ');
        if (end == std::string_view::npos) {
            return -1;
        }
        rest = rest.substr(end);
    }
    long sid = 0;
    std::size_t digits = 0;
    while (digits < rest.size() && rest[digits] >= '0' && rest[digits] <= '9') {
        sid = sid * 10 + (rest[digits] - '0');
        ++digits;
    }
    if (digits == 0 || (digits < rest.size() && rest[digits] != ' ')) {
        return -1;
    }
    return static_cast<pid_t>(sid);
}

std::vector<pid_t> session_members(const std::string& proc_root, pid_t sid) {
    std::vector<pid_t> members;
    DIR* dir = ::opendir(proc_root.c_str());
    if (!dir) {
        return members;
    }
    while (const struct dirent* entry = ::readdir(dir)) {
        const char* name = entry->d_name;
        if (name[0] < '1' || name[0] > '9') {
            continue;
        }
        std::ifstream stat(proc_root + "/" + name + "/stat");
        std::string line;
        if (!std::getline(stat, line) || session_from_proc_stat(line) != sid) {
            continue;
        }
        members.push_back(static_cast<pid_t>(std::strtol(name, nullptr, 10)));
    }
    ::closedir(dir);
    return members;
}

std::string choose_shell(const char* shell_env,
                         const std::function<bool(const std::string&)>& is_executable) {
    if (shell_env && shell_env[0] == '/' && is_executable(shell_env)) {
        return shell_env;
    }
    return "/bin/sh";
}

// ---------------------------------------------------------------------------
// OutputBuffer
// ---------------------------------------------------------------------------

bool OutputBuffer::append(std::string_view bytes, bool& was_empty) {
    std::unique_lock<std::mutex> lock(mutex_);
    space_.wait(lock, [this] { return closed_ || bytes_.size() < cap_; });
    if (closed_) {
        was_empty = false;
        return false;
    }
    was_empty = bytes_.empty();
    bytes_.append(bytes.data(), bytes.size());
    return true;
}

std::string OutputBuffer::take() {
    std::string out;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        out.swap(bytes_);
    }
    space_.notify_all();
    return out;
}

void OutputBuffer::close() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }
    space_.notify_all();
}

std::size_t OutputBuffer::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bytes_.size();
}

// ---------------------------------------------------------------------------
// PtySession
// ---------------------------------------------------------------------------

PtySession::~PtySession() {
    stop();
}

bool PtySession::start(const std::vector<std::string>& argv, const std::vector<std::string>& env,
                       int rows, int cols, WakeFn wake, std::string& error) {
    if (pid_ > 0 || argv.empty()) {
        error = argv.empty() ? "no command" : "already running";
        return false;
    }

    int master = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0) {
        error = std::string("posix_openpt: ") + std::strerror(errno);
        return false;
    }
    if (::grantpt(master) != 0 || ::unlockpt(master) != 0) {
        error = std::string("grantpt/unlockpt: ") + std::strerror(errno);
        ::close(master);
        return false;
    }
    const char* slave_name_c = ::ptsname(master);
    if (!slave_name_c) {
        error = std::string("ptsname: ") + std::strerror(errno);
        ::close(master);
        return false;
    }
    const std::string slave_name = slave_name_c;
    set_winsize(master, rows, cols);

    int wake_pipe[2] = {-1, -1};
    if (::pipe(wake_pipe) != 0) {
        error = std::string("pipe: ") + std::strerror(errno);
        ::close(master);
        return false;
    }
    ::fcntl(wake_pipe[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(wake_pipe[1], F_SETFD, FD_CLOEXEC);

    // Everything the child needs is built before the fork: between fork and exec only
    // async-signal-safe calls are allowed, because another thread may have held the
    // allocator's lock at the moment of the fork.
    std::vector<std::string> env_entries;
    for (char** e = environ; e && *e; ++e) {
        const std::string_view name = env_name(*e);
        const bool overridden = std::any_of(
            env.begin(), env.end(), [&](const std::string& o) { return env_name(o) == name; });
        if (!overridden) {
            env_entries.emplace_back(*e);
        }
    }
    env_entries.insert(env_entries.end(), env.begin(), env.end());
    std::vector<char*> envp;
    for (auto& entry : env_entries) {
        envp.push_back(entry.data());
    }
    envp.push_back(nullptr);
    std::vector<std::string> argv_copy = argv;
    std::vector<char*> argvp;
    for (auto& arg : argv_copy) {
        argvp.push_back(arg.data());
    }
    argvp.push_back(nullptr);
    const char* home = std::getenv("HOME");
    const std::string home_dir = home ? home : "/";

    const pid_t pid = ::fork();
    if (pid < 0) {
        error = std::string("fork: ") + std::strerror(errno);
        ::close(master);
        ::close(wake_pipe[0]);
        ::close(wake_pipe[1]);
        return false;
    }
    if (pid == 0) {
        sigset_t none;
        sigemptyset(&none);
        ::sigprocmask(SIG_SETMASK, &none, nullptr);
        for (int sig : {SIGINT, SIGQUIT, SIGTERM, SIGHUP, SIGPIPE, SIGCHLD, SIGTSTP, SIGTTIN,
                        SIGTTOU, SIGWINCH}) {
            ::signal(sig, SIG_DFL);
        }
        ::setsid();
        const int slave = ::open(slave_name.c_str(), O_RDWR);
        if (slave < 0) {
            ::_exit(126);
        }
        ::ioctl(slave, TIOCSCTTY, 0);
        ::dup2(slave, STDIN_FILENO);
        ::dup2(slave, STDOUT_FILENO);
        ::dup2(slave, STDERR_FILENO);
        for (int fd = STDERR_FILENO + 1; fd < kMaxInheritedFd; ++fd) {
            ::close(fd);
        }
        if (::chdir(home_dir.c_str()) != 0) {
            ::chdir("/");
        }
        ::execve(argvp[0], argvp.data(), envp.data());
        ::_exit(127);
    }

    master_fd_ = master;
    wake_pipe_[0] = wake_pipe[0];
    wake_pipe_[1] = wake_pipe[1];
    pid_ = pid;
    wake_ = std::move(wake);
    hung_up_.store(false, std::memory_order_release);
    stopping_.store(false, std::memory_order_release);
    reader_ = helix::make_thread([this] { read_loop(); });
    return true;
}

void PtySession::read_loop() {
    char buf[kReadChunk];
    while (!stopping_.load(std::memory_order_acquire)) {
        struct pollfd fds[2] = {{master_fd_, POLLIN, 0}, {wake_pipe_[0], POLLIN, 0}};
        const int n = ::poll(fds, 2, -1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (fds[1].revents != 0) {
            break;
        }
        if (fds[0].revents == 0) {
            continue;
        }
        const ssize_t got = ::read(master_fd_, buf, sizeof(buf));
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            // EIO on Linux, 0 elsewhere: the last process holding the slave closed it.
            hung_up_.store(true, std::memory_order_release);
            if (wake_) {
                wake_();
            }
            break;
        }
        bool was_empty = false;
        if (!output_.append(std::string_view(buf, static_cast<std::size_t>(got)), was_empty)) {
            break;
        }
        if (was_empty && wake_) {
            wake_();
        }
    }
}

bool PtySession::write(std::string_view bytes) {
    if (master_fd_ < 0 || hung_up()) {
        return false;
    }
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t n = ::write(master_fd_, bytes.data() + done, bytes.size() - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    return true;
}

void PtySession::resize(int rows, int cols) {
    if (master_fd_ >= 0) {
        set_winsize(master_fd_, rows, cols);
    }
}

int PtySession::stop() {
    if (pid_ <= 0 && !reader_.joinable()) {
        return -1;
    }
    stopping_.store(true, std::memory_order_release);
    output_.close();
    if (wake_pipe_[1] >= 0) {
        char byte = 0;
        [[maybe_unused]] const ssize_t ignored = ::write(wake_pipe_[1], &byte, 1);
    }
    if (reader_.joinable()) {
        reader_.join();
    }
    // Closing the master hangs the terminal up: the kernel sends SIGHUP to the
    // shell's session, and a shell passes it on to its jobs.
    close_fd(master_fd_);
    close_fd(wake_pipe_[0]);
    close_fd(wake_pipe_[1]);

    int exit_code = -1;
    if (pid_ > 0) {
        const pid_t pid = pid_;
        // The shell is its process group's leader and not yet reaped, so the group id
        // still names only its own processes. After the reap it may not.
        ::kill(-pid, SIGHUP);
        const ChildWaitResult wait = terminate_and_reap(
            pid, std::chrono::steady_clock::now() + kExitGrace, kExitGrace, kReapPoll);
        exit_code = wait.reaped ? wait.exit_code() : -1;
        // While any process still names the session, Linux keeps its id from being
        // handed to a new process, so the scan cannot reach an unrelated one.
        for (const pid_t member : session_members("/proc", pid)) {
            if (member != ::getpid()) {
                ::kill(member, SIGKILL);
            }
        }
        pid_ = -1;
    }
    wake_ = nullptr;
    return exit_code;
}

} // namespace helix::terminal

#endif // HELIX_HAS_HOST_TERMINAL
