// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_start_navigation.cpp
 * @brief Tests for the pure auto-navigation gate used by print start navigation.
 *
 * These tests call the REAL helix::print_start_nav_should_navigate() /
 * helix::is_active_print_state() (not shadow copies), so they are the
 * regression guard for the auto-open-print-status gate.
 *
 * The gate must fire on any inactive→active edge, not just →PRINTING:
 * firmware power-loss recovery surfaces the restored job as PAUSED at
 * initial connect (STANDBY→PAUSED), then resumes (#1099). The resume
 * edge itself (PAUSED→PRINTING) must NOT navigate — users who deliberately
 * navigated away mid-print would be yanked back on every resume.
 */

#include "print_start_navigation.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

using helix::is_active_print_state;
using helix::print_start_nav_should_navigate;
using helix::PrintJobState;

TEST_CASE("is_active_print_state: only PRINTING and PAUSED are active", "[print][navigation]") {
    REQUIRE_FALSE(is_active_print_state(PrintJobState::STANDBY));
    REQUIRE(is_active_print_state(PrintJobState::PRINTING));
    REQUIRE(is_active_print_state(PrintJobState::PAUSED));
    REQUIRE_FALSE(is_active_print_state(PrintJobState::COMPLETE));
    REQUIRE_FALSE(is_active_print_state(PrintJobState::CANCELLED));
    REQUIRE_FALSE(is_active_print_state(PrintJobState::ERROR));
}

TEST_CASE("Print start nav: inactive -> active edges navigate", "[print][navigation]") {
    SECTION("STANDBY -> PRINTING (normal print start)") {
        REQUIRE(print_start_nav_should_navigate(PrintJobState::STANDBY, PrintJobState::PRINTING));
    }
    SECTION("STANDBY -> PAUSED (power-loss recovery, #1099)") {
        REQUIRE(print_start_nav_should_navigate(PrintJobState::STANDBY, PrintJobState::PAUSED));
    }
    SECTION("COMPLETE -> PRINTING (retry after finished print)") {
        REQUIRE(print_start_nav_should_navigate(PrintJobState::COMPLETE, PrintJobState::PRINTING));
    }
    SECTION("ERROR -> PAUSED (recovery after error)") {
        REQUIRE(print_start_nav_should_navigate(PrintJobState::ERROR, PrintJobState::PAUSED));
    }
    SECTION("CANCELLED -> PRINTING (new print after cancel)") {
        REQUIRE(print_start_nav_should_navigate(PrintJobState::CANCELLED, PrintJobState::PRINTING));
    }
}

TEST_CASE("Print start nav: active -> active transitions do NOT navigate", "[print][navigation]") {
    SECTION("PAUSED -> PRINTING (mid-session resume must not yank user back)") {
        REQUIRE_FALSE(
            print_start_nav_should_navigate(PrintJobState::PAUSED, PrintJobState::PRINTING));
    }
    SECTION("PRINTING -> PAUSED (pause)") {
        REQUIRE_FALSE(
            print_start_nav_should_navigate(PrintJobState::PRINTING, PrintJobState::PAUSED));
    }
    SECTION("PRINTING -> PRINTING (no edge)") {
        REQUIRE_FALSE(
            print_start_nav_should_navigate(PrintJobState::PRINTING, PrintJobState::PRINTING));
    }
    SECTION("PAUSED -> PAUSED (no edge)") {
        REQUIRE_FALSE(
            print_start_nav_should_navigate(PrintJobState::PAUSED, PrintJobState::PAUSED));
    }
}

TEST_CASE("Print start nav: transitions to inactive states do NOT navigate",
          "[print][navigation]") {
    SECTION("STANDBY -> COMPLETE") {
        REQUIRE_FALSE(
            print_start_nav_should_navigate(PrintJobState::STANDBY, PrintJobState::COMPLETE));
    }
    SECTION("PRINTING -> COMPLETE (print finishes)") {
        REQUIRE_FALSE(
            print_start_nav_should_navigate(PrintJobState::PRINTING, PrintJobState::COMPLETE));
    }
    SECTION("PRINTING -> CANCELLED") {
        REQUIRE_FALSE(
            print_start_nav_should_navigate(PrintJobState::PRINTING, PrintJobState::CANCELLED));
    }
    SECTION("PAUSED -> ERROR") {
        REQUIRE_FALSE(print_start_nav_should_navigate(PrintJobState::PAUSED, PrintJobState::ERROR));
    }
    SECTION("STANDBY -> STANDBY (no edge)") {
        REQUIRE_FALSE(
            print_start_nav_should_navigate(PrintJobState::STANDBY, PrintJobState::STANDBY));
    }
}
