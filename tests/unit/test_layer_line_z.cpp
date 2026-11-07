// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_layer_line_z.cpp
 * @brief Which Z height the print status layer line shows (#2055).
 *
 * Run with: ./build/bin/helix-tests "[layer_line_z]"
 *
 * The numbers model a 6-layer print, 0.25mm first layer and 0.2mm layers
 * (top 1.25mm), whose cancel parks the head at 110.2mm.
 */

#include "layer_line_z.h"

#include "../catch_amalgamated.hpp"

using helix::layer_line_z_centimm;

namespace {
constexpr int kParkZ = 11020;
constexpr double kFirst = 0.25;
constexpr double kHeight = 0.2;
} // namespace

TEST_CASE("Layer line Z: a printing job shows the commanded Z", "[layer_line_z]") {
    REQUIRE(layer_line_z_centimm(true, 65, 3, kFirst, kHeight) == 65);
    // A Z-hop mid-print is still live Z: the geometry is not consulted.
    REQUIRE(layer_line_z_centimm(true, 105, 3, kFirst, kHeight) == 105);
    REQUIRE(layer_line_z_centimm(true, 65, 3, 0.0, 0.0) == 65);
}

TEST_CASE("Layer line Z: an ended job shows the layer's height, not the park height",
          "[layer_line_z]") {
    SECTION("cancelled on layer 1") {
        REQUIRE(layer_line_z_centimm(false, kParkZ, 1, kFirst, kHeight) == 25);
    }
    SECTION("cancelled on layer 4") {
        REQUIRE(layer_line_z_centimm(false, kParkZ, 4, kFirst, kHeight) == 85);
    }
    SECTION("complete: the top layer is the model's top") {
        REQUIRE(layer_line_z_centimm(false, kParkZ, 6, kFirst, kHeight) == 125);
    }
    SECTION("unknown first layer height falls back to the layer height") {
        REQUIRE(layer_line_z_centimm(false, kParkZ, 4, 0.0, kHeight) == 80);
    }
}

TEST_CASE("Layer line Z: an ended job with nothing to compute shows no height", "[layer_line_z]") {
    SECTION("geometry unknown") {
        REQUIRE(layer_line_z_centimm(false, kParkZ, 3, 0.0, 0.0) == 0);
    }
    SECTION("no layer reached") {
        REQUIRE(layer_line_z_centimm(false, kParkZ, 0, kFirst, kHeight) == 0);
    }
}
