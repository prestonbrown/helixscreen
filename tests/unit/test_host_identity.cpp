// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "host_identity.h"

#include <cctype>
#include <string>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

TEST_CASE("host_identity — localhost strings", "[host_identity]") {
    REQUIRE(helix::is_moonraker_on_same_host("localhost"));
    REQUIRE(helix::is_moonraker_on_same_host("127.0.0.1"));
    REQUIRE(helix::is_moonraker_on_same_host("::1"));
    REQUIRE(helix::is_moonraker_on_same_host(""));
}

TEST_CASE("host_identity — loopback literals are case-insensitive", "[host_identity]") {
    // DNS names are case-insensitive, so a host spelled "LocalHost" in config is
    // the same machine; a literal compare answers false here.
    REQUIRE(helix::is_moonraker_on_same_host("LOCALHOST"));
    REQUIRE(helix::is_moonraker_on_same_host("LocalHost"));
}

TEST_CASE("host_identity — loopback-lookalikes are not same-host", "[host_identity]") {
    // Substrings of a loopback literal must not pass.
    REQUIRE_FALSE(helix::is_moonraker_on_same_host("127.0.0.1.example.invalid"));
}

TEST_CASE("host_identity — gethostname matches", "[host_identity]") {
    char buf[256] = {};
    REQUIRE(gethostname(buf, sizeof(buf)) == 0);
    REQUIRE(helix::is_moonraker_on_same_host(buf));

    std::string upper = buf;
    for (auto& c : upper)
        c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    REQUIRE(helix::is_moonraker_on_same_host(upper));
}

TEST_CASE("host_identity — local interface IP matches", "[host_identity]") {
    REQUIRE(helix::is_moonraker_on_same_host("127.0.0.1"));
}

TEST_CASE("host_identity — clearly remote is not same-host", "[host_identity]") {
    REQUIRE_FALSE(helix::is_moonraker_on_same_host("192.0.2.1"));
    REQUIRE_FALSE(helix::is_moonraker_on_same_host("printer.invalid"));
}

TEST_CASE("extract_host_from_websocket_url parses scheme URLs", "[host_identity]") {
    // Standard WebSocket URLs (ws://)
    REQUIRE(helix::extract_host_from_websocket_url("ws://192.168.1.100:7125/websocket") ==
            "192.168.1.100");
    REQUIRE(helix::extract_host_from_websocket_url("ws://localhost:7125/websocket") == "localhost");
    REQUIRE(helix::extract_host_from_websocket_url("ws://127.0.0.1:7125/websocket") == "127.0.0.1");
    REQUIRE(helix::extract_host_from_websocket_url("ws://printer.local:7125/websocket") ==
            "printer.local");

    // Secure WebSocket URLs (wss://)
    REQUIRE(helix::extract_host_from_websocket_url("wss://printer.local:7125/websocket") ==
            "printer.local");
    REQUIRE(helix::extract_host_from_websocket_url("wss://localhost:7125/websocket") ==
            "localhost");
    REQUIRE(helix::extract_host_from_websocket_url("wss://127.0.0.1:7125/websocket") ==
            "127.0.0.1");
    REQUIRE(helix::extract_host_from_websocket_url("wss://192.168.1.100:7125/websocket") ==
            "192.168.1.100");

    // With different ports
    REQUIRE(helix::extract_host_from_websocket_url("ws://localhost:80/websocket") == "localhost");
    REQUIRE(helix::extract_host_from_websocket_url("ws://192.168.1.100:8080/websocket") ==
            "192.168.1.100");
    REQUIRE(helix::extract_host_from_websocket_url("ws://10.0.0.5:7125") == "10.0.0.5");

    // IPv6 URLs (bracketed format)
    REQUIRE(helix::extract_host_from_websocket_url("ws://[::1]:7125/websocket") == "::1");
    REQUIRE(helix::extract_host_from_websocket_url("wss://[::1]:7125/websocket") == "::1");

    REQUIRE(helix::extract_host_from_websocket_url("ws://myhost/websocket") == "myhost");

    // Edge cases
    REQUIRE(helix::extract_host_from_websocket_url("").empty());
    REQUIRE(helix::extract_host_from_websocket_url("invalid").empty());
    REQUIRE(helix::extract_host_from_websocket_url("http://192.168.1.100:8080/")
                .empty()); // unknown scheme
    REQUIRE(helix::extract_host_from_websocket_url("http://not-websocket:7125").empty());
}

TEST_CASE("extract_host_from_websocket_url edge cases", "[host_identity]") {
    SECTION("malformed IPv6 brackets") {
        // Missing closing bracket
        REQUIRE(helix::extract_host_from_websocket_url("ws://[::1:7125/websocket").empty());

        // Empty brackets
        REQUIRE(helix::extract_host_from_websocket_url("ws://[]:7125/websocket").empty());
    }

    SECTION("URLs without port") {
        REQUIRE(helix::extract_host_from_websocket_url("ws://localhost/websocket") == "localhost");
        REQUIRE(helix::extract_host_from_websocket_url("ws://192.168.1.100/path") ==
                "192.168.1.100");
    }

    SECTION("bare hostname") {
        REQUIRE(helix::extract_host_from_websocket_url("ws://localhost") == "localhost");
    }
}
