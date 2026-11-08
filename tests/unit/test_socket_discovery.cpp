// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_socket_discovery.cpp
 * @brief Control-socket liveness detection and multi-instance discovery
 *
 * Unlinking the socket path unconditionally before binding, to clear a file
 * left by an unclean exit, cannot tell a stale file from one a running instance
 * is serving: a second instance would silently steal the path, the first would
 * keep its listener open but become unreachable, and every later client would
 * talk to the newest process instead.
 *
 * These cover the two functions that decide instead. Real AF_UNIX sockets are created
 * on disk, because the entire distinction under test — live listener versus
 * leftover file — is invisible to stat() and cannot be faked.
 */

#include "../../src/remote/unix_socket_transport.h"

#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::UnixSocketTransport;

namespace {

/// Unique scratch directory for one test case.
class TempDir {
  public:
    explicit TempDir(const std::string& tag) {
        path_ = "/tmp/helix-sockdisc-" + tag + "-" + std::to_string(getpid());
        rmdir_recursive();
        mkdir(path_.c_str(), 0700);
    }
    ~TempDir() {
        rmdir_recursive();
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::string& path() const {
        return path_;
    }
    std::string file(const std::string& name) const {
        return path_ + "/" + name;
    }

  private:
    void rmdir_recursive() const {
        std::string cmd = "rm -rf '" + path_ + "'";
        // NOLINTNEXTLINE(cert-env33-c) — fixed, self-constructed path under /tmp
        if (system(cmd.c_str()) != 0) {
            // Best effort; the directory may simply not exist yet.
        }
    }
    std::string path_;
};

/// A real listening AF_UNIX socket, closed and unlinked on destruction.
class Listener {
  public:
    explicit Listener(std::string path) : path_(std::move(path)) {
        fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        REQUIRE(fd_ >= 0);
        struct sockaddr_un addr {};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);
        REQUIRE(bind(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0);
        REQUIRE(listen(fd_, 1) == 0);
    }
    ~Listener() {
        stop();
    }

    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    /// Close the listener but LEAVE the socket file behind — exactly the state an
    /// unclean exit leaves, and the case the old unconditional unlink existed for.
    void abandon() {
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
    }

    void stop() {
        abandon();
        if (!path_.empty()) {
            unlink(path_.c_str());
            path_.clear();
        }
    }

  private:
    std::string path_;
    int fd_ = -1;
};

/// A pid that is certainly not running: fork a child, let it exit, reap it.
///
/// Picking an arbitrary large number instead would be a coin flip — that pid may
/// well be in use, which would silently invert the assertion.
pid_t reaped_pid() {
    pid_t pid = fork();
    if (pid == 0) {
        _exit(0);
    }
    REQUIRE(pid > 0);
    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);
    return pid;
}

/// Path of an instance socket named for @p pid.
std::string sock_for_pid(const TempDir& dir, pid_t pid) {
    return dir.file("helixscreen-control-" + std::to_string(pid) + ".sock");
}

} // namespace

TEST_CASE("Socket sweep: removes a socket whose owning process is dead",
          "[remote][socket_discovery]") {
    // SIGTERM fast-exits without teardown by design, so these files outlive their
    // process. Nothing breaks — discovery probes — but they accumulate forever.
    TempDir dir("sweepdead");
    const std::string path = sock_for_pid(dir, reaped_pid());

    Listener dead(path);
    dead.abandon(); // process gone, file remains
    REQUIRE(access(path.c_str(), F_OK) == 0);

    REQUIRE(UnixSocketTransport::sweep_stale_instances(dir.path()) == 1);
    REQUIRE(access(path.c_str(), F_OK) != 0);
}

TEST_CASE("Socket sweep: spares a socket whose owning process is alive",
          "[remote][socket_discovery]") {
    // Our own pid is unambiguously running. Removing a live instance's path is the
    // exact hijack this branch exists to prevent, so the sweep must never do it.
    TempDir dir("sweeplive");
    const std::string path = sock_for_pid(dir, getpid());

    Listener live(path);
    REQUIRE(UnixSocketTransport::sweep_stale_instances(dir.path()) == 0);
    REQUIRE(access(path.c_str(), F_OK) == 0);
}

TEST_CASE("Socket sweep: spares a bound-but-not-yet-listening socket",
          "[remote][socket_discovery]") {
    // The reason the sweep keys on pid rather than a connect() probe. Between
    // bind() and listen() a connect() gets ECONNREFUSED, so a probe would call
    // this dead and unlink it — stranding an instance that is starting normally.
    TempDir dir("sweepbinding");
    const std::string path = sock_for_pid(dir, getpid());

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    REQUIRE(bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0);
    // Deliberately no listen() yet.
    REQUIRE_FALSE(UnixSocketTransport::path_is_live(path));

    REQUIRE(UnixSocketTransport::sweep_stale_instances(dir.path()) == 0);
    REQUIRE(access(path.c_str(), F_OK) == 0);

    close(fd);
    unlink(path.c_str());
}

TEST_CASE("Socket sweep: spares the bare well-known socket", "[remote][socket_discovery]") {
    // It carries no pid, so the sweep has no evidence about it. Ownership of the
    // well-known path is settled by create_listener's liveness check instead.
    TempDir dir("sweepwellknown");
    const std::string path = dir.file("helixscreen-control.sock");

    Listener stale(path);
    stale.abandon();

    REQUIRE(UnixSocketTransport::sweep_stale_instances(dir.path()) == 0);
    REQUIRE(access(path.c_str(), F_OK) == 0);
}

TEST_CASE("Socket sweep: spares non-numeric and unrelated names", "[remote][socket_discovery]") {
    // A token we did not write is not ours to delete, however dead it looks.
    TempDir dir("sweepnonnumeric");
    Listener named(dir.file("helixscreen-control-debug.sock"));
    Listener wrong_prefix(dir.file("other-control-1234.sock"));
    Listener wrong_suffix(dir.file("helixscreen-control-1234.socket"));
    named.abandon();
    wrong_prefix.abandon();
    wrong_suffix.abandon();

    REQUIRE(UnixSocketTransport::sweep_stale_instances(dir.path()) == 0);
    REQUIRE(access(dir.file("helixscreen-control-debug.sock").c_str(), F_OK) == 0);
    REQUIRE(access(dir.file("other-control-1234.sock").c_str(), F_OK) == 0);
    REQUIRE(access(dir.file("helixscreen-control-1234.socket").c_str(), F_OK) == 0);
}

TEST_CASE("Socket sweep: removes only the dead among several", "[remote][socket_discovery]") {
    TempDir dir("sweepmixed");
    const std::string dead_a = sock_for_pid(dir, reaped_pid());
    const std::string dead_b = sock_for_pid(dir, reaped_pid());
    const std::string alive = sock_for_pid(dir, getpid());

    Listener la(dead_a), lb(dead_b), lc(alive);
    la.abandon();
    lb.abandon();

    REQUIRE(UnixSocketTransport::sweep_stale_instances(dir.path()) == 2);
    REQUIRE(access(dead_a.c_str(), F_OK) != 0);
    REQUIRE(access(dead_b.c_str(), F_OK) != 0);
    REQUIRE(access(alive.c_str(), F_OK) == 0);

    // The survivor is still discoverable — the sweep left the listener untouched.
    auto found = UnixSocketTransport::discover_instances(dir.path());
    REQUIRE(found.size() == 1);
    REQUIRE(found[0] == alive);
}

TEST_CASE("Socket sweep: unreadable directory is a no-op", "[remote][socket_discovery]") {
    REQUIRE(UnixSocketTransport::sweep_stale_instances("/tmp/helix-does-not-exist-xyz") == 0);
    REQUIRE(UnixSocketTransport::sweep_stale_instances("") == 0);
}

TEST_CASE("Socket liveness: a listening socket is live", "[remote][socket_discovery]") {
    TempDir dir("live");
    const std::string path = dir.file("helixscreen-control.sock");

    Listener listener(path);
    REQUIRE(UnixSocketTransport::path_is_live(path));
}

TEST_CASE("Socket liveness: a missing path is not live", "[remote][socket_discovery]") {
    TempDir dir("missing");
    REQUIRE_FALSE(UnixSocketTransport::path_is_live(dir.file("nope.sock")));
    REQUIRE_FALSE(UnixSocketTransport::path_is_live(""));
}

TEST_CASE("Socket liveness: a leftover file with no listener is not live",
          "[remote][socket_discovery]") {
    // The case that motivated the whole change. The file exists, so stat() and
    // access() both say yes; only a connect() attempt reveals nobody is serving it.
    // Reporting this as live would strand a restarting app permanently.
    TempDir dir("stale");
    const std::string path = dir.file("helixscreen-control.sock");

    Listener listener(path);
    REQUIRE(UnixSocketTransport::path_is_live(path));

    listener.abandon(); // closed, file remains
    REQUIRE(access(path.c_str(), F_OK) == 0);
    REQUIRE_FALSE(UnixSocketTransport::path_is_live(path));
}

TEST_CASE("Socket discovery: finds a live pid-suffixed instance", "[remote][socket_discovery]") {
    TempDir dir("one");
    Listener a(dir.file("helixscreen-control-1234.sock"));

    auto found = UnixSocketTransport::discover_instances(dir.path());
    REQUIRE(found.size() == 1);
    REQUIRE(found[0] == dir.file("helixscreen-control-1234.sock"));
}

TEST_CASE("Socket discovery: excludes the bare well-known socket", "[remote][socket_discovery]") {
    // The well-known path is resolved separately; including it here would make a
    // single ordinary instance look like an ambiguous multi-instance situation and
    // force the user to pass -s for no reason.
    TempDir dir("wellknown");
    Listener well_known(dir.file("helixscreen-control.sock"));

    REQUIRE(UnixSocketTransport::discover_instances(dir.path()).empty());
}

TEST_CASE("Socket discovery: skips stale instance files", "[remote][socket_discovery]") {
    TempDir dir("skipstale");
    Listener live(dir.file("helixscreen-control-1111.sock"));
    Listener dead(dir.file("helixscreen-control-2222.sock"));
    dead.abandon(); // file remains, nothing serving it

    auto found = UnixSocketTransport::discover_instances(dir.path());
    REQUIRE(found.size() == 1);
    REQUIRE(found[0] == dir.file("helixscreen-control-1111.sock"));
}

TEST_CASE("Socket discovery: returns several instances sorted", "[remote][socket_discovery]") {
    // With more than one live instance the client must refuse to guess and list
    // them; a stable order keeps that message deterministic.
    TempDir dir("many");
    Listener c(dir.file("helixscreen-control-3333.sock"));
    Listener a(dir.file("helixscreen-control-1111.sock"));
    Listener b(dir.file("helixscreen-control-2222.sock"));

    auto found = UnixSocketTransport::discover_instances(dir.path());
    REQUIRE(found.size() == 3);
    REQUIRE(found[0] == dir.file("helixscreen-control-1111.sock"));
    REQUIRE(found[1] == dir.file("helixscreen-control-2222.sock"));
    REQUIRE(found[2] == dir.file("helixscreen-control-3333.sock"));
}

TEST_CASE("Socket discovery: ignores unrelated files", "[remote][socket_discovery]") {
    TempDir dir("noise");
    Listener real(dir.file("helixscreen-control-9999.sock"));

    // Similar-looking names that must not match.
    Listener wrong_suffix(dir.file("helixscreen-control-4444.socket"));
    Listener wrong_prefix(dir.file("other-control-5555.sock"));

    auto found = UnixSocketTransport::discover_instances(dir.path());
    REQUIRE(found.size() == 1);
    REQUIRE(found[0] == dir.file("helixscreen-control-9999.sock"));
}

TEST_CASE("Socket discovery: unreadable directory yields nothing", "[remote][socket_discovery]") {
    REQUIRE(UnixSocketTransport::discover_instances("/tmp/helix-does-not-exist-xyz").empty());
    REQUIRE(UnixSocketTransport::discover_instances("").empty());
}
