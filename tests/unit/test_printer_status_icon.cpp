// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for PrinterStatusIcon::compute_state — the pure mapping from connection
// + klippy state to the displayed navbar icon. Kept free of LVGL subjects and
// the singleton so the branch logic (including the expected-restart suppression)
// is exercised directly.

#include "ui_printer_status_icon.h"

#include "moonraker_client.h" // ConnectionState
#include "printer_state.h"    // KlippyState

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {
constexpr int CONN_CONNECTED = static_cast<int>(ConnectionState::CONNECTED);
constexpr int CONN_DISCONNECTED = static_cast<int>(ConnectionState::DISCONNECTED);
constexpr int CONN_FAILED = static_cast<int>(ConnectionState::FAILED);

constexpr int KLIPPY_READY = static_cast<int>(KlippyState::READY);
constexpr int KLIPPY_STARTUP = static_cast<int>(KlippyState::STARTUP);
constexpr int KLIPPY_SHUTDOWN = static_cast<int>(KlippyState::SHUTDOWN);
constexpr int KLIPPY_ERROR = static_cast<int>(KlippyState::ERROR);

PrinterIconState state(int conn, int klippy, bool ever_connected, bool expected_restart) {
    return PrinterStatusIcon::compute_state(conn, klippy, ever_connected, expected_restart);
}
} // namespace

TEST_CASE("PrinterStatusIcon::compute_state - connected klippy states", "[status_icon]") {
    SECTION("READY -> READY") {
        REQUIRE(state(CONN_CONNECTED, KLIPPY_READY, true, false) == PrinterIconState::READY);
    }
    SECTION("STARTUP -> WARNING") {
        REQUIRE(state(CONN_CONNECTED, KLIPPY_STARTUP, true, false) == PrinterIconState::WARNING);
    }
    SECTION("SHUTDOWN with no restart pending -> ERROR") {
        REQUIRE(state(CONN_CONNECTED, KLIPPY_SHUTDOWN, true, false) == PrinterIconState::ERROR);
    }
    SECTION("klippy ERROR -> ERROR") {
        REQUIRE(state(CONN_CONNECTED, KLIPPY_ERROR, true, false) == PrinterIconState::ERROR);
    }
}

TEST_CASE("PrinterStatusIcon::compute_state - expected restart suppresses SHUTDOWN error",
          "[status_icon][suppress]") {
    SECTION("transient SHUTDOWN during expected restart shows WARNING, not ERROR") {
        REQUIRE(state(CONN_CONNECTED, KLIPPY_SHUTDOWN, true, /*expected_restart=*/true) ==
                PrinterIconState::WARNING);
    }
    SECTION("expected_restart does NOT mask a genuine klippy ERROR state") {
        // Only the transient SHUTDOWN is softened; a real ERROR still shows red.
        REQUIRE(state(CONN_CONNECTED, KLIPPY_ERROR, true, /*expected_restart=*/true) ==
                PrinterIconState::ERROR);
    }
    SECTION("expected_restart is irrelevant when klippy is READY") {
        REQUIRE(state(CONN_CONNECTED, KLIPPY_READY, true, /*expected_restart=*/true) ==
                PrinterIconState::READY);
    }
}

TEST_CASE("PrinterStatusIcon::compute_state - disconnected states", "[status_icon]") {
    SECTION("connection FAILED -> ERROR") {
        REQUIRE(state(CONN_FAILED, KLIPPY_READY, true, false) == PrinterIconState::ERROR);
    }
    SECTION("disconnected but was connected -> WARNING") {
        REQUIRE(state(CONN_DISCONNECTED, KLIPPY_READY, /*ever_connected=*/true, false) ==
                PrinterIconState::WARNING);
    }
    SECTION("disconnected and never connected -> DISCONNECTED") {
        REQUIRE(state(CONN_DISCONNECTED, KLIPPY_READY, /*ever_connected=*/false, false) ==
                PrinterIconState::DISCONNECTED);
    }
}
