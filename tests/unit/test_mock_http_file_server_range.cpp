// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_mock_http_file_server_range.cpp
 * @brief Pin the mock server's Range behaviour so the #1706 repro harness
 *        stays honest
 *
 * MockHttpFileServer is the stand-in for a Moonraker file server in --test
 * runs; its Range handling is what download_file_partial and the tail
 * scanners exercise. These tests pin the whole path over a real loopback
 * socket: honoured ranges, clamped bounds, 416 for valid-but-empty ranges,
 * and 200 whole-body for malformed headers.
 */

#include "mock_http_file_server.h"

#include <arpa/inet.h>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

namespace {

struct HttpReply {
    int status = 0;
    std::string headers;
    std::string body;
};

std::string to_lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

/// One HTTP/1.0 GET per connection; the server closes the socket after the
/// response, so reading to EOF is the whole reply.
HttpReply http_get(int port, const std::string& path, const std::string& range_header) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

    std::string req = "GET " + path + " HTTP/1.0\r\nHost: 127.0.0.1\r\n";
    if (!range_header.empty()) {
        req += "Range: " + range_header + "\r\n";
    }
    req += "\r\n";
    REQUIRE(::send(fd, req.data(), req.size(), 0) == static_cast<ssize_t>(req.size()));

    std::string raw;
    char buf[4096];
    ssize_t n;
    while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0) {
        raw.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);

    const size_t split = raw.find("\r\n\r\n");
    REQUIRE(split != std::string::npos);
    HttpReply reply;
    reply.status = std::atoi(raw.c_str() + 9); // past "HTTP/1.x "
    reply.headers = to_lower(raw.substr(0, split));
    reply.body = raw.substr(split + 4);
    return reply;
}

} // namespace

TEST_CASE("mock file server honours, clamps and rejects Range headers", "[api][mock][range]") {
    helix::MockHttpFileServer server;
    REQUIRE(server.start());
    const std::string& base = server.base_url();
    const int port = std::atoi(base.c_str() + base.rfind(':') + 1);
    const std::string path = "/server/files/gcodes/mock.png";

    // Derive every expectation from the un-ranged body: the served asset is
    // whatever this tree ships, so nothing here may hardcode its size.
    const HttpReply full = http_get(port, path, "");
    REQUIRE(full.status == 200);
    REQUIRE(full.body.size() > 10);
    const std::string& whole = full.body;

    SECTION("honoured ranges slice the body") {
        const HttpReply head = http_get(port, path, "bytes=0-9");
        REQUIRE(head.status == 206);
        REQUIRE(head.body == whole.substr(0, 10));

        const HttpReply mid = http_get(port, path, "bytes=2-11");
        REQUIRE(mid.status == 206);
        REQUIRE(mid.body == whole.substr(2, 10));

        const HttpReply tail = http_get(port, path, "bytes=-10");
        REQUIRE(tail.status == 206);
        REQUIRE(tail.body == whole.substr(whole.size() - 10));

        const HttpReply open_ended = http_get(port, path, "bytes=2-");
        REQUIRE(open_ended.status == 206);
        REQUIRE(open_ended.body == whole.substr(2));
    }

    SECTION("a bound past the body clamps to the last byte") {
        const HttpReply past_end = http_get(port, path, "bytes=0-99999999");
        REQUIRE(past_end.status == 206);
        REQUIRE(past_end.body == whole);

        const HttpReply long_tail = http_get(port, path, "bytes=-99999999");
        REQUIRE(long_tail.status == 206);
        REQUIRE(long_tail.body == whole);
    }

    SECTION("valid syntax selecting nothing answers 416") {
        // libhv writes a default error page into the 416 body, so the check
        // is the status plus the mandated "bytes */<size>" Content-Range -
        // what a client needs to detect the mismatch - and that the body is
        // not the file.
        const HttpReply start_beyond = http_get(port, path, "bytes=99999999-");
        REQUIRE(start_beyond.status == 416);
        REQUIRE(start_beyond.headers.find("content-range: bytes */" +
                                          std::to_string(whole.size())) != std::string::npos);
        REQUIRE(start_beyond.body != whole);

        const HttpReply zero_suffix = http_get(port, path, "bytes=-0");
        REQUIRE(zero_suffix.status == 416);
        REQUIRE(zero_suffix.headers.find("content-range: bytes */" +
                                         std::to_string(whole.size())) != std::string::npos);
    }

    SECTION("malformed headers are ignored: 200 with the whole body") {
        const HttpReply garbage = http_get(port, path, "bytes=abc-def");
        REQUIRE(garbage.status == 200);
        REQUIRE(garbage.body == whole);

        const HttpReply inverted = http_get(port, path, "bytes=9-2");
        REQUIRE(inverted.status == 200);
        REQUIRE(inverted.body == whole);

        const HttpReply no_dash = http_get(port, path, "bytes=5");
        REQUIRE(no_dash.status == 200);
        REQUIRE(no_dash.body == whole);
    }
}
