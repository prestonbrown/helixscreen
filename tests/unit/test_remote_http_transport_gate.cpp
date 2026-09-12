// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The HTTP transport's two gates, against a real listener: an address
// reachable from off-box does not come up without a token, and a configured
// token is demanded on every request. The pure policy behind both decisions is
// covered in test_remote_http_bind_policy.cpp; these cover the wiring, which a
// pure test cannot reach.

#include "remote_control_server.h"
#include "src/remote/http_transport.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

namespace {

constexpr const char* TOKEN = "9f2c41ab7e0d6835c1a4be97f30d5e62";

/// Parallel shards run concurrently, so each process needs its own port.
int test_port() {
    return 20000 + static_cast<int>(getpid() % 10000);
}

std::string pong_handler(const std::string&) {
    return R"({"jsonrpc":"2.0","result":"pong","id":1})";
}

/// POST a JSON-RPC body to loopback and return the raw response ("" if the
/// port refuses the connection).
std::string post(int port, const std::string& authorization) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return "";
    }
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return "";
    }

    const std::string body = R"({"jsonrpc":"2.0","method":"ping","id":1})";
    std::string req = "POST /rpc HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    if (!authorization.empty()) {
        req += "Authorization: " + authorization + "\r\n";
    }
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    if (write(fd, req.c_str(), req.size()) < 0) {
        close(fd);
        return "";
    }

    std::string response;
    char chunk[1024];
    ssize_t n;
    while ((n = read(fd, chunk, sizeof(chunk))) > 0) {
        response.append(chunk, static_cast<size_t>(n));
    }
    close(fd);
    return response;
}

} // namespace

TEST_CASE("http transport: an off-box bind without a token never listens", "[remote][ctl][http]") {
    const int port = test_port();
    helix::HttpTransport transport("0.0.0.0", port, "");

    const bool started = transport.start(pong_handler);
    // A transport destroyed while its accept thread runs terminates the
    // process, so a regression here has to fail the assertion rather than
    // abort the binary and take the rest of the shard with it.
    if (started) {
        transport.stop();
    }
    REQUIRE_FALSE(started);
    // Nothing is listening, so even loopback cannot reach it.
    REQUIRE(post(port, "").empty());
}

TEST_CASE("http transport: an off-box bind with a token listens", "[remote][ctl][http]") {
    const int port = test_port();
    helix::HttpTransport transport("0.0.0.0", port, TOKEN);

    REQUIRE(transport.start(pong_handler));
    REQUIRE_THAT(post(port, std::string("Bearer ") + TOKEN),
                 Catch::Matchers::ContainsSubstring("pong"));
    transport.stop();
}

TEST_CASE("http transport: a configured token is demanded on every request",
          "[remote][ctl][http]") {
    const int port = test_port();
    // Loopback, which needs no token to bind - so the 401s below can only come
    // from the request check, not from the bind gate.
    helix::HttpTransport transport("127.0.0.1", port, TOKEN);
    REQUIRE(transport.start(pong_handler));

    SECTION("no Authorization header") {
        REQUIRE_THAT(post(port, ""), Catch::Matchers::ContainsSubstring("401"));
    }
    SECTION("wrong token") {
        REQUIRE_THAT(post(port, "Bearer deadbeefdeadbeefdeadbeef"),
                     Catch::Matchers::ContainsSubstring("401"));
    }
    SECTION("correct token") {
        REQUIRE_THAT(post(port, std::string("Bearer ") + TOKEN),
                     Catch::Matchers::ContainsSubstring("pong"));
    }

    transport.stop();
}

TEST_CASE("http transport: no token configured leaves loopback open", "[remote][ctl][http]") {
    const int port = test_port();
    helix::HttpTransport transport("127.0.0.1", port, "");

    REQUIRE(transport.start(pong_handler));
    REQUIRE_THAT(post(port, ""), Catch::Matchers::ContainsSubstring("pong"));
    transport.stop();
}
