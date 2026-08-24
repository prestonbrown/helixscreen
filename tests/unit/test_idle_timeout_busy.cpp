// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_idle_timeout_busy.cpp
 * @brief Debounce Klipper's idle_timeout "Printing" flag before it blocks gcode.
 *
 * idle_timeout.state goes to "Printing" for ANY executing gcode, not just a
 * blocking operation. A printer with housekeeping delayed_gcode loops therefore
 * reports "Printing" in short bursts forever while sitting idle: debug bundle
 * L53W5PKG (Voron Trident, bedfanloop + _AIR_FILTER_TIMER + AFC PREP) logged 632
 * transitions, high for ~0.7 s out of every 10 s. Every one of those windows
 * made the discretionary-gcode guard refuse jogs with "Printer is busy — try
 * again in a moment" on a demonstrably idle printer.
 *
 * Real blocking ops (G28, QGL, BED_MESH_CALIBRATE, PROBE_ACCURACY) hold the flag
 * for many seconds, so duration is the discriminator.
 */

#include "idle_timeout_busy.h"

#include "../catch_amalgamated.hpp"

using helix::IdleTimeoutBusy;

namespace {
using clock = IdleTimeoutBusy::clock;
constexpr clock::time_point T0{};

clock::time_point at_ms(long long ms) {
    return T0 + std::chrono::milliseconds(ms);
}
} // namespace

TEST_CASE("IdleTimeoutBusy: idle printer is never blocking", "[blocking_op][idle_timeout]") {
    IdleTimeoutBusy busy;
    REQUIRE_FALSE(busy.blocking(at_ms(0)));
    REQUIRE_FALSE(busy.blocking(at_ms(60'000)));
}

TEST_CASE("IdleTimeoutBusy: a short housekeeping burst never blocks",
          "[blocking_op][idle_timeout][regression]") {
    // The L53W5PKG shape: Printing at T, Ready 700 ms later, forever.
    IdleTimeoutBusy busy;
    for (int cycle = 0; cycle < 5; ++cycle) {
        const long long base = cycle * 10'000;
        busy.set_printing(true, at_ms(base));
        INFO("cycle " << cycle);
        // Queried at any point inside the burst, it must stay clear.
        CHECK_FALSE(busy.blocking(at_ms(base)));
        CHECK_FALSE(busy.blocking(at_ms(base + 400)));
        CHECK_FALSE(busy.blocking(at_ms(base + 699)));
        busy.set_printing(false, at_ms(base + 700));
        CHECK_FALSE(busy.blocking(at_ms(base + 700)));
    }
}

TEST_CASE("IdleTimeoutBusy: a sustained operation blocks once settled",
          "[blocking_op][idle_timeout]") {
    IdleTimeoutBusy busy;
    busy.set_printing(true, at_ms(0));

    SECTION("not yet at the threshold") {
        CHECK_FALSE(busy.blocking(at_ms(999)));
    }
    SECTION("exactly at the threshold") {
        CHECK(busy.blocking(at_ms(1000)));
    }
    SECTION("and stays blocking for the whole operation") {
        CHECK(busy.blocking(at_ms(1500)));
        CHECK(busy.blocking(at_ms(45'000)));
    }
}

TEST_CASE("IdleTimeoutBusy: clearing releases immediately, no debounce on the way down",
          "[blocking_op][idle_timeout]") {
    // Asymmetric on purpose: making the user wait an extra second after a real
    // homing finishes would be a second bug, and there is no false-negative to
    // protect against on this edge.
    IdleTimeoutBusy busy;
    busy.set_printing(true, at_ms(0));
    REQUIRE(busy.blocking(at_ms(5000)));

    busy.set_printing(false, at_ms(5001));
    CHECK_FALSE(busy.blocking(at_ms(5001)));
}

TEST_CASE("IdleTimeoutBusy: a repeated Printing report does not restart the timer",
          "[blocking_op][idle_timeout][regression]") {
    // Moonraker re-sends idle_timeout in status batches. If every repeat rearmed
    // the settle window, a printer that reports "Printing" more often than once
    // a second would never reach the blocking state at all — turning the
    // debounce into a permanent hole in the guard.
    IdleTimeoutBusy busy;
    busy.set_printing(true, at_ms(0));
    busy.set_printing(true, at_ms(300));
    busy.set_printing(true, at_ms(600));
    busy.set_printing(true, at_ms(900));
    CHECK(busy.blocking(at_ms(1000)));
}

TEST_CASE("IdleTimeoutBusy: re-entering Printing restarts the settle window",
          "[blocking_op][idle_timeout]") {
    IdleTimeoutBusy busy;
    busy.set_printing(true, at_ms(0));
    busy.set_printing(false, at_ms(700));
    busy.set_printing(true, at_ms(10'000));

    CHECK_FALSE(busy.blocking(at_ms(10'500)));
    CHECK(busy.blocking(at_ms(11'000)));
}
