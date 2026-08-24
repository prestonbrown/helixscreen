// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_format_utils.h"

#include "../helix_test_fixture.h"

#include "../catch_amalgamated.hpp"

using helix::ui::format_layer_progress;

TEST_CASE_METHOD(HelixTestFixture, "format_layer_progress renders total when known",
                 "[format][layer]") {
    REQUIRE(format_layer_progress(42, 213, true, 0) == "Layer 42 / 213");
}

TEST_CASE_METHOD(HelixTestFixture, "format_layer_progress omits an unknown total",
                 "[format][layer]") {
    // A zero total means Moonraker/the slicer never supplied a layer count.
    // "Layer 7 / 0" would be worse than saying nothing about the total.
    REQUIRE(format_layer_progress(7, 0, true, 0) == "Layer 7");
    REQUIRE(format_layer_progress(7, -1, true, 0) == "Layer 7");
}

TEST_CASE_METHOD(HelixTestFixture, "format_layer_progress marks estimated counts with ~",
                 "[format][layer]") {
    REQUIRE(format_layer_progress(42, 213, false, 0) == "Layer ~42 / 213");
    REQUIRE(format_layer_progress(7, 0, false, 0) == "Layer ~7");
}

TEST_CASE_METHOD(HelixTestFixture, "format_layer_progress appends Z height when available",
                 "[format][layer]") {
    // Z arrives in centimillimeters and renders as millimeters to one decimal.
    REQUIRE(format_layer_progress(42, 213, true, 2400) == "Layer 42 / 213 (24.0mm)");
    REQUIRE(format_layer_progress(42, 213, false, 2450) == "Layer ~42 / 213 (24.5mm)");
    REQUIRE(format_layer_progress(7, 0, true, 1005) == "Layer 7 (10.1mm)");
}

TEST_CASE_METHOD(HelixTestFixture, "format_layer_progress omits a missing Z height",
                 "[format][layer]") {
    REQUIRE(format_layer_progress(42, 213, true, 0) == "Layer 42 / 213");
    REQUIRE(format_layer_progress(42, 213, true, -5) == "Layer 42 / 213");
}
