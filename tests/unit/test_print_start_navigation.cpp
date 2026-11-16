// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_start_navigation.cpp
 * @brief Tests for the pure auto-navigation gate used by print start navigation.
 *
 * These tests call the REAL helix::print_start_nav_should_navigate() /
 * helix::printer_has_job() (not shadow copies), so they are the
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

using helix::print_start_nav_should_navigate;
using helix::printer_has_job;
using helix::PrintJobState;

TEST_CASE("printer_has_job: only PRINTING and PAUSED count", "[print][navigation]") {
    REQUIRE_FALSE(printer_has_job(PrintJobState::STANDBY));
    REQUIRE(printer_has_job(PrintJobState::PRINTING));
    REQUIRE(printer_has_job(PrintJobState::PAUSED));
    REQUIRE_FALSE(printer_has_job(PrintJobState::COMPLETE));
    REQUIRE_FALSE(printer_has_job(PrintJobState::CANCELLED));
    REQUIRE_FALSE(printer_has_job(PrintJobState::ERROR));
}

TEST_CASE("Print start nav: only an inactive -> active edge navigates", "[print][navigation]") {
    using S = PrintJobState;
    struct Edge {
        S prev, current;
        bool navigates;
    };
    const Edge edges[] = {
        {S::STANDBY, S::PRINTING, true},   // normal print start
        {S::STANDBY, S::PAUSED, true},     // power-loss recovery (#1099)
        {S::COMPLETE, S::PRINTING, true},  // retry after a finished print
        {S::ERROR, S::PAUSED, true},       // recovery after an error
        {S::CANCELLED, S::PRINTING, true}, // new print after a cancel
        {S::PAUSED, S::PRINTING, false},   // a resume must not yank the user back
        {S::PRINTING, S::PAUSED, false},   {S::PRINTING, S::PRINTING, false},
        {S::PAUSED, S::PAUSED, false},     {S::STANDBY, S::COMPLETE, false},
        {S::PRINTING, S::COMPLETE, false}, {S::PRINTING, S::CANCELLED, false},
        {S::PAUSED, S::ERROR, false},      {S::STANDBY, S::STANDBY, false},
    };
    for (const Edge& e : edges) {
        CAPTURE(static_cast<int>(e.prev), static_cast<int>(e.current));
        CHECK(print_start_nav_should_navigate(e.prev, e.current) == e.navigates);
    }
}
