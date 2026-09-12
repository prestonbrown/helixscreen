// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Bind and auth policy for the remote-control HTTP transport.
//
// The endpoint exposes the full ctl command set and carries no authentication
// of its own, so a listener reachable from off-box may only come up when a
// token is configured to gate it. Both rules are pure functions so the policy
// is exercised without opening a socket.

#include "remote_control_server.h"

#include "../catch_amalgamated.hpp"

using helix::decide_http_bind;
using helix::http_token_matches;
using helix::HttpBindDecision;

namespace {
// 32 hex chars - the shape `openssl rand -hex 16` produces.
constexpr const char* STRONG = "9f2c41ab7e0d6835c1a4be97f30d5e62";
} // namespace

// --- bind policy --------------------------------------------------------

TEST_CASE("http bind: loopback needs no token", "[remote][ctl][http]") {
    REQUIRE(decide_http_bind("127.0.0.1", "") == HttpBindDecision::Allow);
    // The whole 127/8 is loopback, not just .0.0.1.
    REQUIRE(decide_http_bind("127.0.0.53", "") == HttpBindDecision::Allow);
    REQUIRE(decide_http_bind("127.255.255.254", "") == HttpBindDecision::Allow);
}

TEST_CASE("http bind: off-box bind is refused without a token", "[remote][ctl][http]") {
    REQUIRE(decide_http_bind("0.0.0.0", "") == HttpBindDecision::TokenRequired);
    REQUIRE(decide_http_bind("192.168.1.50", "") == HttpBindDecision::TokenRequired);
    REQUIRE(decide_http_bind("10.0.0.5", "") == HttpBindDecision::TokenRequired);
}

TEST_CASE("http bind: a token admits an off-box bind", "[remote][ctl][http]") {
    REQUIRE(decide_http_bind("0.0.0.0", STRONG) == HttpBindDecision::Allow);
    REQUIRE(decide_http_bind("192.168.1.50", STRONG) == HttpBindDecision::Allow);
}

TEST_CASE("http bind: a guessable token does not admit an off-box bind", "[remote][ctl][http]") {
    REQUIRE(decide_http_bind("0.0.0.0", "hunter2") == HttpBindDecision::TokenTooWeak);
    // 15 bytes - one short of the floor.
    REQUIRE(decide_http_bind("0.0.0.0", "0123456789abcde") == HttpBindDecision::TokenTooWeak);
    // 16 bytes - the floor itself is accepted.
    REQUIRE(decide_http_bind("0.0.0.0", "0123456789abcdef") == HttpBindDecision::Allow);
    // A weak token is irrelevant to a loopback bind, which never needed one.
    REQUIRE(decide_http_bind("127.0.0.1", "hunter2") == HttpBindDecision::Allow);
}

TEST_CASE("http bind: the host must be a numeric IPv4 address", "[remote][ctl][http]") {
    // create_listener() feeds the host to inet_pton, which resolves nothing.
    REQUIRE(decide_http_bind("localhost", "") == HttpBindDecision::InvalidHost);
    REQUIRE(decide_http_bind("", "") == HttpBindDecision::InvalidHost);
    REQUIRE(decide_http_bind("999.1.1.1", "") == HttpBindDecision::InvalidHost);
    REQUIRE(decide_http_bind("::1", STRONG) == HttpBindDecision::InvalidHost);
}

// --- request auth -------------------------------------------------------

TEST_CASE("http auth: the configured bearer token is accepted", "[remote][ctl][http]") {
    REQUIRE(http_token_matches(STRONG, std::string("Bearer ") + STRONG));
    // RFC 7235 auth schemes are case-insensitive.
    REQUIRE(http_token_matches(STRONG, std::string("bearer ") + STRONG));
    REQUIRE(http_token_matches(STRONG, std::string("BEARER ") + STRONG));
}

TEST_CASE("http auth: anything else is rejected", "[remote][ctl][http]") {
    REQUIRE_FALSE(http_token_matches(STRONG, ""));
    REQUIRE_FALSE(http_token_matches(STRONG, "Bearer wrong"));
    // A correct token with no scheme is still not a bearer credential.
    REQUIRE_FALSE(http_token_matches(STRONG, STRONG));
    REQUIRE_FALSE(http_token_matches(STRONG, std::string("Basic ") + STRONG));
    // A prefix of the token must not pass.
    REQUIRE_FALSE(http_token_matches(STRONG, "Bearer 9f2c41ab"));
}

TEST_CASE("http auth: an unconfigured token fails closed", "[remote][ctl][http]") {
    // serve_client() skips the check when no token is set; if it ever stops
    // skipping, an empty expectation must not match an empty header.
    REQUIRE_FALSE(http_token_matches("", ""));
    REQUIRE_FALSE(http_token_matches("", "Bearer "));
}
