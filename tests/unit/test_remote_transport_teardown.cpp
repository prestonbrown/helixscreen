// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Destroying a remote-control transport that is still running.
//
// std::thread's destructor terminates the process when it holds a joinable
// thread, so a transport must shut its accept thread down on the way out
// rather than rely on every caller remembering to stop() first.

#include "src/remote/http_transport.h"
#include "src/remote/unix_socket_transport.h"

#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

namespace {

/// Parallel shards run concurrently, so each process needs its own names.
int test_port() {
    return 30000 + static_cast<int>(getpid() % 10000);
}

std::string test_socket_path() {
    return "/tmp/helix-teardown-" + std::to_string(getpid()) + ".sock";
}

std::string pong_handler(const std::string&) {
    return R"({"jsonrpc":"2.0","result":"pong","id":1})";
}

bool socket_file_exists(const std::string& path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0;
}

} // namespace

TEST_CASE("transport teardown: destroying a running listener frees its port",
          "[remote][ctl][http]") {
    const int port = test_port();
    {
        helix::HttpTransport transport("127.0.0.1", port, "");
        REQUIRE(transport.start(pong_handler));
    } // destroyed while running, with no stop() call

    // Rebinding proves the listener fd was closed and the accept thread joined.
    helix::HttpTransport again("127.0.0.1", port, "");
    REQUIRE(again.start(pong_handler));
    again.stop();
}

TEST_CASE("transport teardown: the subclass destructor still reaches on_stopped()",
          "[remote][ctl]") {
    // The base class can only join the thread - by the time it runs, the
    // derived object is gone and on_stopped() no longer dispatches. Unlinking
    // the socket file is UnixSocketTransport::on_stopped()'s job, so a leftover
    // file means the teardown happened one level too late.
    const std::string path = test_socket_path();
    unlink(path.c_str());

    {
        helix::UnixSocketTransport transport(path);
        REQUIRE(transport.start(pong_handler));
        REQUIRE(socket_file_exists(path));
    }

    REQUIRE_FALSE(socket_file_exists(path));
}

TEST_CASE("transport teardown: stop() before destruction stays idempotent", "[remote][ctl]") {
    const std::string path = test_socket_path();
    unlink(path.c_str());

    helix::UnixSocketTransport transport(path);
    REQUIRE(transport.start(pong_handler));
    transport.stop();
    REQUIRE_FALSE(socket_file_exists(path));
    // A second stop(), then the destructor's own teardown, must both no-op.
    transport.stop();
}
