// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "printer_versions_state.h"

#include <string>

#include "../catch_amalgamated.hpp"

// The predicate decides whether the remap card carries a "history will keep the
// rewritten name" note, so every answer it can give is worth pinning: an absent
// version must not read as old, and a git-describe suffix must not drag a
// supported release below the floor.
TEST_CASE("Moonraker history floor", "[version][moonraker]") {
    SECTION("releases below the floor warn") {
        CHECK(helix::moonraker_history_is_degraded("v0.8.0"));
        CHECK(helix::moonraker_history_is_degraded("0.8.9"));
        CHECK(helix::moonraker_history_is_degraded("v0.7.1"));
    }

    SECTION("the floor and above do not") {
        CHECK_FALSE(helix::moonraker_history_is_degraded("v0.9.0"));
        CHECK_FALSE(helix::moonraker_history_is_degraded("0.9.3"));
        CHECK_FALSE(helix::moonraker_history_is_degraded("v1.0.0"));
    }

    SECTION("a git-describe suffix is compared on the core triple") {
        // SemVer ranks a prerelease below its own release, so comparing the
        // full string would warn a printer running a build of the floor
        // release itself.
        CHECK_FALSE(helix::moonraker_history_is_degraded("v0.9.0-16-g0f1e2d3"));
        CHECK_FALSE(helix::moonraker_history_is_degraded("v0.9.4-2-gdeadbee"));
        CHECK(helix::moonraker_history_is_degraded("v0.8.0-16-g0f1e2d3"));
    }

    SECTION("a version we could not read is not a version we can judge") {
        // server.info's moonraker_version defaults to "unknown" when the field
        // is missing. Treating an unreadable string as too old would warn every
        // printer that never reported one.
        CHECK_FALSE(helix::moonraker_history_is_degraded("unknown"));
        CHECK_FALSE(helix::moonraker_history_is_degraded(""));
        CHECK_FALSE(helix::moonraker_history_is_degraded("not-a-version"));
    }
}
