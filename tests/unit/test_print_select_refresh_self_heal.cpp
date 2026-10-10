// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_select_refresh_self_heal.cpp
 * @brief Lock in PrintSelectPanel's directory-error self-heal behavior.
 *
 * The stuck-refresh self-heal decision that used to live here as the
 * refresh_should_skip() predicate (#911) has been extracted into
 * helix::InFlightGuard — see test_in_flight_guard.cpp ([inflight]) for that
 * coverage. What remains here is dir_error_should_reset_to_root(), the
 * phantom-directory fallback that is still a pure predicate on the panel.
 */

#include "ui_panel_print_select.h"

#include <chrono>

#include "../catch_amalgamated.hpp"

using namespace std::chrono_literals;

// ============================================================================
// dir_error_should_reset_to_root: recover from a phantom current directory.
//
// FlashForge's Moonraker returned a doubled folder segment
// ("Feinkost/Feinkost/Gridfinity") that the server reports as missing; the panel
// retried it every refresh forever (debug bundle TJVQDCZ6). When get_directory
// fails because the directory is gone, the panel should fall back to root rather
// than loop. Resetting only when NOT already at root prevents an infinite
// reset->refresh->fail cycle if root itself ever errors.
// ============================================================================

TEST_CASE("dir_error_should_reset_to_root: missing-directory error in subfolder resets",
          "[print_select][refresh][regression]") {
    REQUIRE(dir_error_should_reset_to_root(
                "Directory does not exist (/usr/data/gcodes/Feinkost/Feinkost/Gridfinity)",
                /*at_root=*/false) == true);
}

TEST_CASE("dir_error_should_reset_to_root: already at root never resets (no loop)",
          "[print_select][refresh][regression]") {
    REQUIRE(dir_error_should_reset_to_root("Directory does not exist (/usr/data/gcodes)",
                                           /*at_root=*/true) == false);
}

TEST_CASE("dir_error_should_reset_to_root: unrelated error does not reset",
          "[print_select][refresh][regression]") {
    REQUIRE(dir_error_should_reset_to_root("Klippy disconnected", /*at_root=*/false) == false);
    REQUIRE(dir_error_should_reset_to_root("", /*at_root=*/false) == false);
}

TEST_CASE("dir_error_should_reset_to_root: match is case-insensitive",
          "[print_select][refresh][regression]") {
    REQUIRE(dir_error_should_reset_to_root("DIRECTORY DOES NOT EXIST (/x)",
                                           /*at_root=*/false) == true);
}

// ============================================================================
// connection_change_should_refresh: re-list on a reconnect, not on registration.
//
// The connection observer's first notification fires when it is registered and
// reports the state the panel was built in. On the K-Touch the panel is built on
// its first visit, already connected, and on_activate() lists right after; a
// forced refresh here supersedes that request and the first fill waits for a
// second listing.
// ============================================================================

namespace {
constexpr int kNoneYet = -1;
constexpr int state(helix::ConnectionState s) {
    return static_cast<int>(s);
}
} // namespace

TEST_CASE("connection_change_should_refresh: registration while connected does not refresh",
          "[print_select][refresh]") {
    REQUIRE_FALSE(helix::connection_change_should_refresh(
        kNoneYet, state(helix::ConnectionState::CONNECTED)));
}

TEST_CASE("connection_change_should_refresh: every way back to CONNECTED refreshes",
          "[print_select][refresh]") {
    using helix::ConnectionState;
    for (auto from : {ConnectionState::DISCONNECTED, ConnectionState::CONNECTING,
                      ConnectionState::RECONNECTING, ConnectionState::FAILED}) {
        CAPTURE(static_cast<int>(from));
        REQUIRE(helix::connection_change_should_refresh(state(from),
                                                        state(ConnectionState::CONNECTED)));
    }
}

TEST_CASE("connection_change_should_refresh: anything but arriving at CONNECTED does not",
          "[print_select][refresh]") {
    using helix::ConnectionState;
    REQUIRE_FALSE(helix::connection_change_should_refresh(state(ConnectionState::CONNECTED),
                                                          state(ConnectionState::CONNECTED)));
    REQUIRE_FALSE(helix::connection_change_should_refresh(state(ConnectionState::CONNECTED),
                                                          state(ConnectionState::RECONNECTING)));
    REQUIRE_FALSE(
        helix::connection_change_should_refresh(kNoneYet, state(ConnectionState::DISCONNECTED)));
}
