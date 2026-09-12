// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Which directory holds the `helix-screen ctl` control socket.
//
// The shipped systemd unit sets ProtectSystem=strict and declares
// RuntimeDirectory=helixscreen, so /run/helixscreen is the only writable
// directory it has and /tmp is read-only. Resolving to /tmp there fails the
// bind on every systemd install (prestonbrown/helixscreen#1602), and the
// failure is non-fatal, so the app carries on looking healthy.

#include "../test_helpers/scoped_env.h"
#include "remote_control_server.h"
#include "src/remote/unix_socket_transport.h"

#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

using helix::control_socket_dir;
using helix::ScopedEnv;
using helix::well_known_socket_path;

namespace {
/// Both knobs are process-global; every case sets both so none inherits one.
struct EnvSandbox {
    ScopedEnv runtime{"RUNTIME_DIRECTORY"};
    ScopedEnv xdg{"XDG_RUNTIME_DIR"};
};
} // namespace

TEST_CASE("control socket dir: systemd's RUNTIME_DIRECTORY wins", "[remote][ctl][socketdir]") {
    EnvSandbox sandbox;
    setenv("RUNTIME_DIRECTORY", "/run/helixscreen", 1);
    setenv("XDG_RUNTIME_DIR", "/run/user/1000", 1);

    REQUIRE(control_socket_dir() == "/run/helixscreen");
}

TEST_CASE("control socket dir: a colon-separated list yields the first entry",
          "[remote][ctl][socketdir]") {
    EnvSandbox sandbox;
    // systemd joins multiple RuntimeDirectory= entries with ':'.
    setenv("RUNTIME_DIRECTORY", "/run/helixscreen:/run/helixscreen-extra", 1);
    unsetenv("XDG_RUNTIME_DIR");

    REQUIRE(control_socket_dir() == "/run/helixscreen");
}

TEST_CASE("control socket dir: an empty RUNTIME_DIRECTORY falls through",
          "[remote][ctl][socketdir]") {
    EnvSandbox sandbox;
    setenv("RUNTIME_DIRECTORY", "", 1);
    setenv("XDG_RUNTIME_DIR", "/run/user/1000", 1);

    REQUIRE(control_socket_dir() == "/run/user/1000");
}

TEST_CASE("control socket dir: XDG_RUNTIME_DIR is the desktop case", "[remote][ctl][socketdir]") {
    EnvSandbox sandbox;
    unsetenv("RUNTIME_DIRECTORY");
    setenv("XDG_RUNTIME_DIR", "/run/user/1000", 1);

    REQUIRE(control_socket_dir() == "/run/user/1000");
}

TEST_CASE("control socket dir: /tmp only when nothing else is set", "[remote][ctl][socketdir]") {
    EnvSandbox sandbox;
    unsetenv("RUNTIME_DIRECTORY");
    unsetenv("XDG_RUNTIME_DIR");

    REQUIRE(control_socket_dir() == "/tmp");
}

TEST_CASE("control socket dir: the well-known path sits inside it", "[remote][ctl][socketdir]") {
    EnvSandbox sandbox;
    setenv("RUNTIME_DIRECTORY", "/run/helixscreen", 1);
    unsetenv("XDG_RUNTIME_DIR");

    REQUIRE(well_known_socket_path() == "/run/helixscreen/helixscreen-control.sock");
}

TEST_CASE("resolve_socket_path: the server binds inside RUNTIME_DIRECTORY",
          "[remote][ctl][socketdir]") {
    EnvSandbox sandbox;
    // A directory that exists: resolve_socket_path() sweeps it for the sockets
    // of instances that died without teardown before it picks a path.
    const std::string dir = "/tmp/helix-rd-" + std::to_string(getpid());
    mkdir(dir.c_str(), 0700);
    setenv("RUNTIME_DIRECTORY", dir.c_str(), 1);
    unsetenv("XDG_RUNTIME_DIR");

    REQUIRE(helix::resolve_socket_path("") == dir + "/helixscreen-control.sock");
    // An explicit --remote-socket still outranks everything.
    REQUIRE(helix::resolve_socket_path("/tmp/explicit.sock") == "/tmp/explicit.sock");

    rmdir(dir.c_str());
}
