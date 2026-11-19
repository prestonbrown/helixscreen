// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../helix_test_fixture.h"
#include "display_format.h"

#include "../catch_amalgamated.hpp"

using helix::display_format::format_layer_progress_compact;
using helix::display_format::format_layer_z;

TEST_CASE_METHOD(HelixTestFixture, "format_layer_progress_compact renders total when known",
                 "[format][layer]") {
    REQUIRE(format_layer_progress_compact(42, 213, true, 0) == "42 / 213");
}

TEST_CASE_METHOD(HelixTestFixture, "format_layer_progress_compact omits an unknown total",
                 "[format][layer]") {
    // A zero total means Moonraker/the slicer never supplied a layer count.
    // "7 / 0" would be worse than saying nothing about the total.
    REQUIRE(format_layer_progress_compact(7, 0, true, 0) == "7");
    REQUIRE(format_layer_progress_compact(7, -1, true, 0) == "7");
}

TEST_CASE_METHOD(HelixTestFixture, "format_layer_progress_compact marks estimated counts with ~",
                 "[format][layer]") {
    REQUIRE(format_layer_progress_compact(42, 213, false, 0) == "~42 / 213");
    REQUIRE(format_layer_progress_compact(7, 0, false, 0) == "~7");
}

TEST_CASE_METHOD(HelixTestFixture, "format_layer_progress_compact appends Z height when available",
                 "[format][layer]") {
    // A layers icon stands where prose would spell "Layer", and a middle dot
    // separates the Z. The string must stay translation-neutral: its width is
    // the row's budget.
    REQUIRE(format_layer_progress_compact(474, 1334, true, 5540) == "474 / 1334 · 55.4mm");
    REQUIRE(format_layer_progress_compact(42, 213, false, 2450) == "~42 / 213 · 24.5mm");
    REQUIRE(format_layer_progress_compact(7, 0, true, 1005) == "7 · 10.1mm");
    REQUIRE(format_layer_progress_compact(42, 213, true, -5) == "42 / 213");
}

TEST_CASE_METHOD(HelixTestFixture, "format_layer_z renders Z in millimeters or nothing",
                 "[format][layer]") {
    // Z arrives in centimillimeters and renders as millimeters to one decimal.
    REQUIRE(format_layer_z(2400) == "· 24.0mm");
    REQUIRE(format_layer_z(1005) == "· 10.1mm");
    REQUIRE(format_layer_z(0).empty());
    REQUIRE(format_layer_z(-5).empty());
}
