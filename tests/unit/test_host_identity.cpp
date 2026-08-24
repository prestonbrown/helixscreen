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
    // the same machine. The three call sites folded into this predicate used to
    // compare literally and answered false here.
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
