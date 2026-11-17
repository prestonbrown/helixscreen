// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_remote_screen_pairing.cpp
 * @brief Remote control's pure rules: touch parsing, the request gate, pairing and
 *        the stale-press guard.
 */

#include "remote_screen_control.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <set>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace std::chrono_literals;

namespace {

struct FakeClock {
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::time_point{} + 1h;
    PairingStore::Clock fn() {
        return [this] { return now; };
    }
};

struct Saved {
    std::string code;
    std::vector<std::string> tokens;
    int code_saves = 0;
    PairingStore::Persist persist() {
        return {[this](const std::string& c) {
                    code = c;
                    ++code_saves;
                },
                [this](const std::vector<std::string>& t) { tokens = t; }};
    }
};

} // namespace

TEST_CASE("parse_touch_body accepts the three types and rejects bad input",
          "[remote_screen][control]") {
    auto down = parse_touch_body(R"({"type":"down","x":10,"y":20})", 800, 480);
    REQUIRE(down.has_value());
    CHECK(down->type == TouchEvent::Type::Down);
    CHECK(down->x == 10);
    CHECK(down->y == 20);
    CHECK(parse_touch_body(R"({"type":"move","x":0,"y":0})", 800, 480)->type ==
          TouchEvent::Type::Move);
    CHECK(parse_touch_body(R"({"type":"up","x":799,"y":479})", 800, 480)->type ==
          TouchEvent::Type::Up);

    CHECK_FALSE(parse_touch_body(R"({"type":"tap","x":1,"y":1})", 800, 480));
    CHECK_FALSE(parse_touch_body(R"({"type":"down","y":1})", 800, 480));
    CHECK_FALSE(parse_touch_body(R"({"type":"down","x":"1","y":1})", 800, 480));
    CHECK_FALSE(parse_touch_body(R"({"type":"down","x":1.5,"y":1})", 800, 480));
    CHECK_FALSE(parse_touch_body(R"({"type":"down","x":800,"y":1})", 800, 480));
    CHECK_FALSE(parse_touch_body(R"({"type":"down","x":-1,"y":1})", 800, 480));
    CHECK_FALSE(parse_touch_body("not json", 800, 480));
    CHECK_FALSE(parse_touch_body(R"({"type":"down","x":1,"y":1})", 0, 0));
}

TEST_CASE("check_touch_request gates control, CSRF, origin and pairing",
          "[remote_screen][control]") {
    const std::string json = "application/json";
    const std::string host = "192.168.1.9:7130";
    CHECK(check_touch_request(false, false, json, "1", "", host, "", false) ==
          TouchGate::ControlOff);
    CHECK(check_touch_request(true, false, "text/plain", "1", "", host, "", false) ==
          TouchGate::BadContentType);
    CHECK(check_touch_request(true, false, json, "", "", host, "", false) ==
          TouchGate::MissingHeader);
    CHECK(check_touch_request(true, false, json, "1", "http://evil.example", host, "", false) ==
          TouchGate::OriginMismatch);
    CHECK(check_touch_request(true, true, json, "1", "http://" + host, host, "", false) ==
          TouchGate::NotPaired);
    CHECK(check_touch_request(true, true, json, "1", "http://" + host, host, "", true) ==
          TouchGate::Allowed);
    // Behind nginx, auth_request already decided: no cookie needed.
    CHECK(check_touch_request(true, false, json, "1", "", host, "", false) == TouchGate::Allowed);
    // A charset parameter on the content type is still JSON.
    CHECK(check_touch_request(true, false, "application/json; charset=utf-8", "1", "", host, "",
                              false) == TouchGate::Allowed);
}

TEST_CASE("a forwarded host counts for the origin check only behind nginx",
          "[remote_screen][control]") {
    const std::string json = "application/json";
    // A reverse proxy at https://proxy.example/printer/ in front of the printer's nginx:
    // the browser's Origin names the proxy, Host names the printer.
    const std::string origin = "https://proxy.example";
    const std::string host = "printer.local";

    SECTION("X-Forwarded-Host: the first entry is the one the browser addressed") {
        CHECK(trusted_forwarded_host(true, "proxy.example", "") == "proxy.example");
        CHECK(trusted_forwarded_host(true, "proxy.example, inner.lan", "") == "proxy.example");
        CHECK(trusted_forwarded_host(true, " proxy.example:8443 ", "") == "proxy.example:8443");
    }
    SECTION("Forwarded: host= of the first element, quoted or not") {
        CHECK(trusted_forwarded_host(true, "", "for=10.0.0.2;host=proxy.example;proto=https") ==
              "proxy.example");
        CHECK(trusted_forwarded_host(true, "",
                                     R"(proto=https;Host="proxy.example:8443", )"
                                     R"(host=inner.lan)") == "proxy.example:8443");
        CHECK(trusted_forwarded_host(true, "", "for=10.0.0.2;proto=https").empty());
        CHECK(trusted_forwarded_host(true, "proxy.example", "host=other.example") ==
              "proxy.example");
    }
    SECTION("a server facing the LAN ignores both headers") {
        CHECK(trusted_forwarded_host(false, "proxy.example", "").empty());
        CHECK(trusted_forwarded_host(false, "", "host=proxy.example").empty());
    }
    SECTION("through nginx, an Origin naming the forwarded host is accepted") {
        const std::string fwd = trusted_forwarded_host(true, "proxy.example", "");
        CHECK(check_touch_request(true, false, json, "1", origin, host, fwd, false) ==
              TouchGate::Allowed);
        // Any other origin is still refused.
        CHECK(check_touch_request(true, false, json, "1", "https://evil.example", host, fwd,
                                  false) == TouchGate::OriginMismatch);
    }
    SECTION("a direct request with a mismatched Origin is refused whatever it forwards") {
        const std::string fwd = trusted_forwarded_host(false, "proxy.example", "");
        CHECK(check_touch_request(true, true, json, "1", origin, host, fwd, true) ==
              TouchGate::OriginMismatch);
    }
    SECTION("through nginx without forwarding headers the mismatch is still refused") {
        CHECK(check_touch_request(true, false, json, "1", origin, host, "", false) ==
              TouchGate::OriginMismatch);
    }
    SECTION("an accepted forwarded origin on the LAN still needs pairing") {
        CHECK(check_touch_request(true, true, json, "1", origin, host, "proxy.example", false) ==
              TouchGate::NotPaired);
    }
}

TEST_CASE("pairing: right code mints a token that is_paired accepts; wrong code does not",
          "[remote_screen][control]") {
    FakeClock clock;
    Saved saved;
    PairingStore store("123456", {}, saved.persist(), clock.fn());
    std::string token;
    CHECK(store.try_pair("654321", token) == PairingStore::PairResult::WrongCode);
    CHECK(token.empty());
    REQUIRE(store.try_pair("123456", token) == PairingStore::PairResult::Paired);
    CHECK(token.size() == 32);
    CHECK(store.is_paired(token));
    CHECK_FALSE(store.is_paired("0123456789abcdef0123456789abcdef"));
    CHECK_FALSE(store.is_paired(""));
    CHECK(saved.tokens == std::vector<std::string>{token});
}

TEST_CASE("pairing: 6th wrong code inside 60s is RateLimited, a right code later pairs",
          "[remote_screen][control]") {
    FakeClock clock;
    Saved saved;
    PairingStore store("123456", {}, saved.persist(), clock.fn());
    std::string token;
    for (int i = 0; i < kPairAttemptsPerWindow; ++i) {
        CHECK(store.try_pair("000000", token) == PairingStore::PairResult::WrongCode);
    }
    CHECK(store.try_pair("123456", token) == PairingStore::PairResult::RateLimited);
    CHECK(store.retry_after_seconds() > 0);
    clock.now += std::chrono::milliseconds(kPairWindowMs + 1000);
    CHECK(store.retry_after_seconds() == 0);
    CHECK(store.try_pair("123456", token) == PairingStore::PairResult::Paired);
}

TEST_CASE("pairing: 50 cumulative failures rotate the code", "[remote_screen][control]") {
    FakeClock clock;
    Saved saved;
    PairingStore store("123456", {}, saved.persist(), clock.fn());
    std::string token;
    for (int i = 0; i < kPairFailuresBeforeRotate; ++i) {
        store.try_pair("000000", token);
        clock.now += std::chrono::milliseconds(kPairWindowMs / kPairAttemptsPerWindow + 1000);
    }
    CHECK(store.code() != "123456");
    CHECK(saved.code == store.code());
    CHECK(store.try_pair("123456", token) == PairingStore::PairResult::WrongCode);
}

TEST_CASE("pairing: 9th token evicts the least recently used", "[remote_screen][control]") {
    FakeClock clock;
    Saved saved;
    PairingStore store("123456", {}, saved.persist(), clock.fn());
    std::vector<std::string> tokens;
    for (size_t i = 0; i < kMaxPairedBrowsers; ++i) {
        std::string t;
        REQUIRE(store.try_pair("123456", t) == PairingStore::PairResult::Paired);
        tokens.push_back(t);
    }
    // Using the first one makes the second the least recently used.
    CHECK(store.is_paired(tokens[0]));
    std::string ninth;
    REQUIRE(store.try_pair("123456", ninth) == PairingStore::PairResult::Paired);
    CHECK(store.is_paired(tokens[0]));
    CHECK_FALSE(store.is_paired(tokens[1]));
    CHECK(store.is_paired(ninth));
    CHECK(saved.tokens.size() == kMaxPairedBrowsers);
}

TEST_CASE("pairing: forget_all clears tokens and changes the code", "[remote_screen][control]") {
    Saved saved;
    PairingStore store("123456", {"aaaa"}, saved.persist());
    CHECK(store.is_paired("aaaa"));
    store.forget_all();
    CHECK_FALSE(store.is_paired("aaaa"));
    CHECK(store.code() != "123456");
    CHECK(saved.tokens.empty());
    CHECK(saved.code == store.code());
}

TEST_CASE("pairing: an empty stored code is generated and saved", "[remote_screen][control]") {
    Saved saved;
    PairingStore store("", {}, saved.persist());
    CHECK(store.code().size() == 6);
    CHECK(saved.code == store.code());
}

TEST_CASE("generate_code is 6 digits", "[remote_screen][control]") {
    std::set<std::string> seen;
    for (int i = 0; i < 1000; ++i) {
        const std::string c = PairingStore::generate_code();
        REQUIRE(c.size() == 6);
        REQUIRE(std::all_of(c.begin(), c.end(), [](char ch) { return ch >= '0' && ch <= '9'; }));
        seen.insert(c);
    }
    // 1000 draws from a million codes: collisions are rare, a constant is not.
    CHECK(seen.size() > 990);
}

TEST_CASE("a down with no follow-up is released after kTouchStaleMs", "[remote_screen][control]") {
    TouchSession s;
    const auto t0 = std::chrono::steady_clock::time_point{} + 1h;
    s.on_event({TouchEvent::Type::Down, 10, 20}, t0);
    CHECK_FALSE(s.take_stale_release(t0 + std::chrono::milliseconds(kTouchStaleMs - 1)));
    s.on_event({TouchEvent::Type::Move, 15, 25}, t0 + 1000ms);
    CHECK_FALSE(s.take_stale_release(t0 + std::chrono::milliseconds(kTouchStaleMs + 500)));
    auto rel = s.take_stale_release(t0 + 1000ms + std::chrono::milliseconds(kTouchStaleMs));
    REQUIRE(rel.has_value());
    CHECK(rel->type == TouchEvent::Type::Up);
    CHECK(rel->x == 15);
    CHECK(rel->y == 25);
    // Released once.
    CHECK_FALSE(s.take_stale_release(t0 + 10s));

    // A completed tap never needs releasing.
    TouchSession tap;
    tap.on_event({TouchEvent::Type::Down, 1, 1}, t0);
    tap.on_event({TouchEvent::Type::Up, 1, 1}, t0 + 50ms);
    CHECK_FALSE(tap.take_stale_release(t0 + 10s));
}

TEST_CASE("control_active goes 1 on an accepted touch and 0 after kViewerIdleMs",
          "[remote_screen][control]") {
    const auto t0 = std::chrono::steady_clock::time_point{} + 1h;
    CHECK_FALSE(remote_control_active(std::nullopt, t0));
    CHECK(remote_control_active(t0, t0));
    CHECK(remote_control_active(t0, t0 + std::chrono::milliseconds(kRemoteControlActiveMs - 1)));
    CHECK_FALSE(remote_control_active(t0, t0 + std::chrono::milliseconds(kRemoteControlActiveMs)));
}

TEST_CASE("a move with nothing pressed is not accepted", "[remote_screen][control]") {
    // After a stale press is released, trackpad jitter must not start a new press.
    TouchSession s;
    const auto t0 = std::chrono::steady_clock::time_point{} + 1h;
    CHECK_FALSE(s.on_event({TouchEvent::Type::Move, 5, 5}, t0));
    CHECK(s.on_event({TouchEvent::Type::Down, 5, 5}, t0));
    CHECK(s.on_event({TouchEvent::Type::Move, 6, 6}, t0));
    REQUIRE(s.take_stale_release(t0 + std::chrono::milliseconds(kTouchStaleMs + 1)));
    CHECK_FALSE(s.on_event({TouchEvent::Type::Move, 7, 7}, t0 + 5s));
    CHECK(s.on_event({TouchEvent::Type::Up, 7, 7}, t0 + 5s));
}
