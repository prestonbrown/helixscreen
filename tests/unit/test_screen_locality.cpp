// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "screen_locality.h"

#include <cstring>

#include "../catch_amalgamated.hpp"

TEST_CASE("screen_locality_for_host reports local for loopback", "[telemetry][locality]") {
    CHECK(std::strcmp(helix::screen_locality_for_host("localhost"), "local") == 0);
    CHECK(std::strcmp(helix::screen_locality_for_host("127.0.0.1"), "local") == 0);
    CHECK(std::strcmp(helix::screen_locality_for_host("::1"), "local") == 0);
}

TEST_CASE("screen_locality_for_host reports remote for an off-box address",
          "[telemetry][locality]") {
    // 192.0.2.0/24 is TEST-NET-1 (RFC 5737) - guaranteed never assigned to a
    // real interface, so this cannot accidentally match a build machine's IP.
    CHECK(std::strcmp(helix::screen_locality_for_host("192.0.2.1"), "remote") == 0);
}

TEST_CASE("screen_locality_for_host reports remote for an empty host", "[telemetry][locality]") {
    // An unconfigured host is not evidence of co-location. Defaulting to
    // "local" would inflate exactly the statistic this field exists to measure.
    CHECK(std::strcmp(helix::screen_locality_for_host(""), "remote") == 0);
}
