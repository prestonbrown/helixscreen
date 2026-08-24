// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_lane_requirement.cpp
 * @brief The rule the pre-print gate and the filament mapping card share.
 *
 * It decides whether a print's lane mapping means anything. The gate uses it to
 * compare against the external spool instead of the lanes; the card uses it to
 * decide whether offering a mapping is a real control or a claim the print will
 * ignore. Both must read the same precedence, which is why the rule is exported
 * rather than written twice.
 */

#include "print_start_checks.h"

#include <set>

#include "../catch_amalgamated.hpp"

using helix::print_lane_requirement;

TEST_CASE("lane requirement: the scan wins over the palette", "[print_start][lanes]") {
    // The case that motivated the fallback ordering, and the file that exposed
    // it on a K2 Plus: OrcaSlicer emitted PLA;ASA-GF;ASA-GF;PLA for a print
    // sliced against T1 alone. Counting the palette calls that a 4-lane print
    // and keeps the mapping card up on a bypass print that uses none of it.
    CHECK(print_lane_requirement({1}, 4) == 1);
    CHECK(print_lane_requirement({0, 2}, 4) == 2);
}

TEST_CASE("lane requirement: the palette is only a not-yet-scanned fallback",
          "[print_start][lanes]") {
    // Empty tools_used means the G-code scan has not run, not "no tools".
    CHECK(print_lane_requirement({}, 4) == 4);
    CHECK(print_lane_requirement({}, 1) == 1);
    CHECK(print_lane_requirement({}, 0) == 0);
}

TEST_CASE("lane requirement: single-tool files are not lane prints", "[print_start][lanes]") {
    // <= 1 is what both callers branch on, so pin the boundary from both sides.
    CHECK(print_lane_requirement({0}, 1) <= 1);
    CHECK(print_lane_requirement({3}, 8) <= 1);
    CHECK_FALSE(print_lane_requirement({0, 1}, 2) <= 1);
}
